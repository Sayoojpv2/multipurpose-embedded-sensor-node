/* comms.c - CRCs, LoRa airtime model and link layer with store-and-forward. */
#include "comms.h"
#include <math.h>
#include <string.h>

/* ---------------- CRC ---------------- */
uint16_t crc16_ccitt_false(const uint8_t *d, size_t len)
{
    uint16_t crc = 0xFFFF;
    while (len--) {
        crc ^= (uint16_t)(*d++) << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
    return crc;
}

uint8_t crc8_sensirion(const uint8_t *d, size_t len)
{
    uint8_t crc = 0xFF;
    while (len--) {
        crc ^= *d++;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x31u) : (uint8_t)(crc << 1);
    }
    return crc;
}

/* ---------------- LoRa time on air ----------------
 * Semtech SX1276 datasheet / AN1200.13 formula:
 *  Tsym = 2^SF / BW
 *  Tpre = (Npre + 4.25) * Tsym
 *  Nsym = 8 + max(ceil((8PL - 4SF + 28 + 16CRC - 20IH) / (4(SF - 2DE))) * (CR + 4), 0)
 * explicit header (IH = 0); DE (low data-rate optimisation) for SF >= 11 @125 kHz. */
float lora_airtime_ms(const lora_cfg_t *c, uint16_t pl, bool crc_on)
{
    float tsym = (float)(1u << c->sf) / (float)c->bw_hz * 1000.0f;
    int de = (c->sf >= 11 && c->bw_hz <= 125000u) ? 1 : 0;
    float num = 8.0f * pl - 4.0f * c->sf + 28.0f + (crc_on ? 16.0f : 0.0f);
    float den = 4.0f * (float)(c->sf - 2 * de);
    float nsym = ceilf(num / den) * (float)(c->cr + 4);
    if (nsym < 0) nsym = 0;
    return ((float)c->preamble + 4.25f) * tsym + (8.0f + nsym) * tsym;
}

void lora_default_cfg(lora_cfg_t *c)
{
    c->sf = 9; c->bw_hz = 125000u; c->cr = 1; c->preamble = 8; c->mac_overhead = 13;
}

/* ---------------- link layer ---------------- */
uint32_t xorshift32(uint32_t *s)
{
    uint32_t x = *s ? *s : 0x9E3779B9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}

void link_default_cfg(link_cfg_t *c)
{
    c->max_retries = 3;
    c->backoff_init_s = 120; c->backoff_max_s = 1800;
    c->backoff_urgent_max_s = 300;
    c->max_per_service = 6;
    c->stored_age_s = 30;
    c->rx_window_ms = 2 * 30.0f;   /* RX1 + RX2 preamble-detect timeouts (assumed) */
    c->ack_len = 12;               /* empty confirmed-downlink frame               */
}

void link_init(link_t *l, const link_cfg_t *c, const lora_cfg_t *lc, uint32_t seed)
{
    memset(l, 0, sizeof(*l));
    l->cfg = *c; l->lora = *lc;
    l->up = true; l->backoff_s = c->backoff_init_s;
    l->rng = seed ? seed : 1u;
}

static int pick_next(const link_t *l)
{
    int best = -1;
    for (int i = 0; i < l->count; i++) {
        const txq_item_t *a = &l->q[i];
        if (best < 0) { best = i; continue; }
        const txq_item_t *b = &l->q[best];
        if (a->prio > b->prio || (a->prio == b->prio && a->created_t < b->created_t) ||
            (a->prio == b->prio && a->created_t == b->created_t && a->seq < b->seq))
            best = i;
    }
    return best;
}

int link_submit(link_t *l, const uint8_t *buf, uint8_t len, uint8_t prio, uint16_t seq, uint8_t type, uint32_t now)
{
    l->st.submitted++;
    if (len > PKT_MAX_LEN) return -1;
    int slot;
    if (l->count < TXQ_CAP) {
        slot = l->count++;
    } else {                       /* queue full: evict lowest priority, oldest */
        int v = 0;
        for (int i = 1; i < l->count; i++)
            if (l->q[i].prio < l->q[v].prio || (l->q[i].prio == l->q[v].prio && l->q[i].created_t < l->q[v].created_t)) v = i;
        l->st.dropped_queue++;
        if (l->q[v].prio > prio) return -2;   /* new packet is the least important */
        slot = v;
    }
    txq_item_t *it = &l->q[slot];
    memcpy(it->buf, buf, len);
    it->len = len; it->prio = prio; it->seq = seq; it->type = type;
    it->created_t = now; it->waited_outage = !l->up;
    /* an alert while the link is down: try it at the next service call, do not wait for the backoff */
    if (!l->up && prio == PRIO_CRITICAL && l->next_probe_t > now) l->next_probe_t = now;
    if (l->count > l->st.max_queue) l->st.max_queue = l->count;
    return 0;
}

/* A packet occupies uplink airtime plus its ACK receive interval (or
 * timeout). Retries add backoff; queued packets use one shared timeline. */
static bool send_item(link_t *l, txq_item_t *it, uint32_t now)
{
    uint64_t t_ms = (uint64_t)now * 1000u;
    if (t_ms < l->next_tx_ms) t_ms = l->next_tx_ms;

    for (uint8_t a = 0; a <= l->cfg.max_retries; a++) {
        uint32_t tx_s = (uint32_t)(t_ms / 1000u);
        if (tx_s >= it->created_t && tx_s - it->created_t >= l->cfg.stored_age_s)
            packet_set_flags(it->buf, it->len, PKT_FLAG_STORED);

        float air = lora_airtime_ms(&l->lora, (uint16_t)(it->len + l->lora.mac_overhead), true);
        bool acked = radio_transmit(it->buf, it->len, t_ms, air) == 1;
        float rx = acked ? lora_airtime_ms(&l->lora, l->cfg.ack_len, false) : l->cfg.rx_window_ms;

        l->st.attempts++;
        if (a > 0) l->st.retries++;
        l->st.bytes_air += it->len;
        l->st.airtime_ms += air;
        l->st.rx_ms += rx;
        if (l->on_attempt) l->on_attempt(l->cb_ctx, tx_s, it, a, air, acked);

        t_ms += (uint64_t)ceilf(air + rx);
        if (acked) {
            l->next_tx_ms = t_ms;
            return true;
        }
        if (a < l->cfg.max_retries)
            t_ms += (uint64_t)(2u + xorshift32(&l->rng) % 5u) * 1000u;
    }
    l->next_tx_ms = t_ms;
    return false;
}

void link_service(link_t *l, uint32_t now)
{
    if (!l->up && now < l->next_probe_t) return;
    uint8_t sent = 0;
    while (l->count > 0 && sent < l->cfg.max_per_service) {
        int i = pick_next(l);
        txq_item_t *it = &l->q[i];
        if (send_item(l, it, now)) {
            l->st.delivered++;
            l->st.bytes_delivered += it->len;
            if (it->waited_outage) l->st.recovered_after_outage++;
            l->q[i] = l->q[l->count - 1];
            l->count--;
            sent++;
            if (!l->up) { l->up = true; l->backoff_s = l->cfg.backoff_init_s; }
        } else {
            l->st.failed_sends++;
            if (l->up) {
                l->up = false; l->st.outages++;
                for (int k = 0; k < l->count; k++) l->q[k].waited_outage = true;
            }
            /* exponential backoff; probe more often while urgent data is waiting */
            uint32_t cap = l->cfg.backoff_max_s;
            for (int k = 0; k < l->count; k++)
                if (l->q[k].prio == PRIO_CRITICAL && l->cfg.backoff_urgent_max_s < cap) cap = l->cfg.backoff_urgent_max_s;
            if (l->backoff_s > cap) l->backoff_s = cap;
            l->next_probe_t = (uint32_t)((l->next_tx_ms + 999u) / 1000u) + l->backoff_s;
            l->backoff_s = (l->backoff_s * 2 > cap) ? cap : l->backoff_s * 2;
            return;
        }
    }
}

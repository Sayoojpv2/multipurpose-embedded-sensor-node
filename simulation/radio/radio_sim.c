/* radio_sim.c - implements the firmware's radio_transmit() with a channel
 * model: outage windows from the trace, Bernoulli uplink loss, random bit
 * corruption (caught by the gateway CRC-16) and Bernoulli ACK loss.
 * gateway: CRC check, decode, duplicate suppression, logging. */
#include "radio_sim.h"
#include "comms.h"
#include "../sensors/virtual_sensors.h"
#include <string.h>

static radio_sim_cfg_t g_cfg = {0.0f, 0.0f, 0.0f, 1};
static radio_sim_stats_t g_st;
static uint32_t g_rng = 1;

static float urand(void) { return (float)(xorshift32(&g_rng) >> 8) / 16777216.0f; }

void radio_sim_init(const radio_sim_cfg_t *c)
{
    g_cfg = *c; memset(&g_st, 0, sizeof g_st);
    g_rng = c->seed ? c->seed * 747796405u + 1u : 1u;
}

const radio_sim_stats_t *radio_sim_stats(void) { return &g_st; }

int radio_transmit(const uint8_t *buf, uint8_t len, uint32_t t)
{
    g_st.tx++;
    if (!env_link_up(t)) { g_st.lost_outage++; return 0; }
    if (urand() < g_cfg.p_loss) { g_st.lost_channel++; return 0; }
    uint8_t rx[PKT_MAX_LEN];
    memcpy(rx, buf, len);
    if (urand() < g_cfg.p_corrupt) {
        uint32_t bit = xorshift32(&g_rng) % (uint32_t)(len * 8u);
        rx[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        g_st.corrupted++;
    }
    if (!gateway_receive(rx, len, t)) return 0;          /* CRC failure: no ACK */
    if (urand() < g_cfg.p_ack_loss) { g_st.ack_lost++; return 0; }
    return 1;
}

/* ---------------- gateway ---------------- */
#define GW_SEEN 256
static gw_log_cb g_cb;
static void *g_ctx;
static gw_stats_t g_gw;
static uint32_t g_seen[GW_SEEN];   /* (node<<16 | seq) + 1, 0 = empty */
static uint32_t g_seen_idx;

void gateway_init(gw_log_cb cb, void *ctx)
{
    g_cb = cb; g_ctx = ctx;
    memset(&g_gw, 0, sizeof g_gw); memset(g_seen, 0, sizeof g_seen); g_seen_idx = 0;
}

const gw_stats_t *gateway_stats(void) { return &g_gw; }

int gateway_receive(const uint8_t *buf, uint8_t len, uint32_t t)
{
    packet_t p;
    int r = packet_decode(buf, len, &p);
    if (r == -2) { g_gw.crc_fail++; return 0; }
    if (r != 0)  { g_gw.decode_fail++; return 0; }
    uint32_t key = ((uint32_t)p.node_id << 16 | p.seq) + 1u;
    for (int i = 0; i < GW_SEEN; i++)
        if (g_seen[i] == key) { g_gw.duplicates++; return 1; }   /* ACK again, do not log twice */
    g_seen[g_seen_idx] = key; g_seen_idx = (g_seen_idx + 1) % GW_SEEN;
    g_gw.rx_ok++; g_gw.bytes += len;
    if (g_cb) g_cb(g_ctx, t, &p, len);
    return 1;
}

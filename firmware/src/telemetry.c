/* telemetry.c - reporting policy.
 *
 * FIXED     : every sample -> DATA packet with all channels.
 * ADAPTIVE* : event-driven
 *   1. node becomes CRITICAL (or a new channel becomes critical) -> ALERT now
 *   2. still CRITICAL after crit_repeat_s                         -> ALERT repeat
 *   3. any channel changes state (warning/recovery/fault/normal)  -> UPDATE / FAULT
 *   4. a channel moved >= delta_report since last report          -> UPDATE (changed channels only)
 *      (rate-limited by min_update_s)
 *   5. nothing sent and summary_s elapsed                         -> SUMMARY (mean/min/max)
 * Raw samples are never discarded: every sample is in local storage.
 */
#include "node.h"
#include <math.h>
#include <string.h>

static void acc_reset(telemetry_t *t)
{
    for (int i = 0; i < MAX_CHANNELS; i++) { t->sum[i] = 0; t->cnt[i] = 0; t->mn[i] = INFINITY; t->mx[i] = -INFINITY; }
}

void telemetry_init(telemetry_t *t)
{
    memset(t, 0, sizeof(*t));
    acc_reset(t);
}

static void fill_channels(packet_t *pk, const app_profile_t *p, uint8_t mask, const float *filt,
                          const sensor_reading_t *rd, const uint8_t *ch_state)
{
    pk->mask = mask;
    for (int i = 0; i < p->n_channels; i++) {
        bool v = rd[i].status == RD_OK;
        pk->ch[i].sensor_id = p->ch[i].sensor_id;
        pk->ch[i].status = pkt_status_byte(ch_state[i], v, rd[i].status);
        pk->ch[i].value = v ? filt[i] : NAN;
        pk->ch[i].vmin = pk->ch[i].vmax = pk->ch[i].value;
    }
}

static uint8_t prio_for_state(uint8_t s)
{
    if (s == EV_CRITICAL) return PRIO_CRITICAL;
    if (s == EV_WARNING || s == EV_FAULT) return PRIO_HIGH;
    return PRIO_NORMAL;
}

/* Remember what the gateway now knows, so later deltas are measured
 * against the last transmitted value rather than the last sample. */
static void mark_reported(telemetry_t *t, int nc, uint8_t mask, const float *filt, const sensor_reading_t *rd)
{
    for (int i = 0; i < nc; i++)
        if ((mask & (1u << i)) && rd[i].status == RD_OK) { t->last_reported[i] = filt[i]; t->has_reported[i] = true; }
}

static void fill_summary(telemetry_t *t, packet_t *pk, int nc)
{
    for (int i = 0; i < nc; i++) {
        bool any = t->cnt[i] > 0;
        pk->ch[i].value = any ? t->sum[i] / (float)t->cnt[i] : NAN;
        pk->ch[i].vmin = any ? t->mn[i] : NAN;
        pk->ch[i].vmax = any ? t->mx[i] : NAN;
        if (any) { t->last_reported[i] = pk->ch[i].value; t->has_reported[i] = true; }
    }
}

/* Adaptive policy, highest priority rule first. Returns 1 if *pk was filled. */
static int adaptive_decide(telemetry_t *t, const app_profile_t *p, uint32_t now, const float *filt,
                           const sensor_reading_t *rd, const uint8_t *ch_state, uint8_t node_state, packet_t *pk)
{
    int nc = p->n_channels;
    uint8_t changed = 0, crit_new = 0, abnormal = 0, delta = 0, faulted = 0;
    for (int i = 0; i < nc; i++) {
        uint8_t bit = (uint8_t)(1u << i);
        if (ch_state[i] != t->last_ch_state[i]) changed |= bit;
        if (ch_state[i] == EV_CRITICAL && t->last_ch_state[i] != EV_CRITICAL) crit_new |= bit;
        if (ch_state[i] == EV_FAULT && t->last_ch_state[i] != EV_FAULT) faulted |= bit;
        if (ch_state[i] == EV_CRITICAL || ch_state[i] == EV_WARNING) abnormal |= bit;
        if (rd[i].status == RD_OK &&
            (!t->has_reported[i] || fabsf(filt[i] - t->last_reported[i]) >= p->ch[i].delta_report))
            delta |= bit;
    }

    if (node_state == EV_CRITICAL && (crit_new || t->last_node_state != EV_CRITICAL)) {
        pk->type = PKT_ALERT; pk->priority = PRIO_CRITICAL;               /* 1. new critical */
        fill_channels(pk, p, (uint8_t)(abnormal | changed | delta), filt, rd, ch_state);
        t->last_alert_t = now;
    } else if (node_state == EV_CRITICAL && now - t->last_alert_t >= p->telemetry.crit_repeat_s) {
        pk->type = PKT_ALERT; pk->priority = PRIO_HIGH;                   /* 2. still critical */
        fill_channels(pk, p, (uint8_t)(abnormal | delta), filt, rd, ch_state);
        t->last_alert_t = now;
    } else if (changed) {                                                 /* 3. state change */
        pk->type = faulted ? PKT_FAULT : PKT_UPDATE;
        pk->priority = faulted ? PRIO_HIGH : prio_for_state(node_state);
        fill_channels(pk, p, (uint8_t)(changed | delta), filt, rd, ch_state);
    } else if (delta && now - t->last_report_t >= p->telemetry.min_update_s) {
        pk->type = PKT_UPDATE; pk->priority = PRIO_NORMAL;                /* 4. value moved */
        fill_channels(pk, p, delta, filt, rd, ch_state);
    } else if (now - t->last_summary_t >= p->telemetry.summary_s) {
        pk->type = PKT_SUMMARY; pk->priority = PRIO_LOW;                  /* 5. heartbeat */
        fill_channels(pk, p, (uint8_t)((1u << nc) - 1u), filt, rd, ch_state);
        fill_summary(t, pk, nc);
        t->last_summary_t = now;
        acc_reset(t);
        return 1;              /* a summary does not reset the update rate limit */
    } else {
        return 0;
    }
    t->last_report_t = now;
    mark_reported(t, nc, pk->mask, filt, rd);
    return 1;
}

int telemetry_on_sample(telemetry_t *t, const app_profile_t *p, uint8_t strategy, uint32_t now,
                        const float *filt, const sensor_reading_t *rd, const uint8_t *ch_state,
                        uint8_t node_state, packet_t *pk)
{
    int nc = p->n_channels;
    for (int i = 0; i < nc; i++) {                 /* statistics for the next SUMMARY */
        if (rd[i].status != RD_OK) continue;
        t->sum[i] += filt[i]; t->cnt[i]++;
        if (filt[i] < t->mn[i]) t->mn[i] = filt[i];
        if (filt[i] > t->mx[i]) t->mx[i] = filt[i];
    }
    if (!t->started) {
        t->started = true; t->last_summary_t = now;
        t->last_node_state = EV_NORMAL;
        for (int i = 0; i < nc; i++) t->last_ch_state[i] = EV_NORMAL;
    }

    memset(pk, 0, sizeof(*pk));
    pk->timestamp = now; pk->node_state = node_state;
    int n;
    if (strategy == STRAT_FIXED) {                 /* baseline: every sample, all channels */
        uint8_t all = (uint8_t)((1u << nc) - 1u);
        pk->type = PKT_DATA; pk->priority = prio_for_state(node_state);
        fill_channels(pk, p, all, filt, rd, ch_state);
        t->last_report_t = now;
        mark_reported(t, nc, all, filt, rd);
        n = 1;
    } else {
        n = adaptive_decide(t, p, now, filt, rd, ch_state, node_state, pk);
    }

    t->last_node_state = node_state;
    for (int i = 0; i < nc; i++) t->last_ch_state[i] = ch_state[i];
    if (n == 0) t->suppressed++;
    return n;
}

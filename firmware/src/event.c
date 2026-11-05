/* event.c - threshold event detection with hysteresis and confirmation. */
#include "event.h"

void event_init(chan_event_t *e)
{
    e->state = EV_NORMAL; e->pend_level = 0;
    e->up_count = e->down_count = e->rec_count = e->fault_count = 0;
    e->transitions = 0;
}

/* Level of a value: 0 normal, 1 warning, 2 critical.
 * relax > 0 moves the limits towards normal by 'relax' (used for hysteresis:
 * a channel that is already at a level stays there until the value is
 * inside the limit by more than the hysteresis band). */
int event_level(const thresholds_t *t, float v, float relax)
{
    if (v >= t->crit_hi - relax || v <= t->crit_lo + relax) return 2;
    if (v >= t->warn_hi - relax || v <= t->warn_lo + relax) return 1;
    return 0;
}

static int state_level(uint8_t s)
{
    return s == EV_CRITICAL ? 2 : (s == EV_WARNING ? 1 : 0);
}

static void set_state(chan_event_t *e, uint8_t s)
{
    if (e->state != s) { e->state = s; e->transitions++; }
    e->up_count = e->down_count = 0;
    if (s == EV_RECOVERY || s == EV_FAULT) e->rec_count = 0;
}

uint8_t event_update(chan_event_t *e, const thresholds_t *t, const event_cfg_t *c, bool valid, float v)
{
    if (!valid) {
        if (e->fault_count < 255) e->fault_count++;
        if (e->fault_count >= c->fault_n && e->state != EV_FAULT) set_state(e, EV_FAULT);
        return e->state;                       /* hold state on missing data */
    }
    e->fault_count = 0;
    if (e->state == EV_FAULT) { set_state(e, EV_RECOVERY); }

    int cur = state_level(e->state);
    int up = event_level(t, v, 0.0f);
    int hold = event_level(t, v, t->hyst);     /* relaxed limits (hysteresis) */
    int cand = up > cur ? up : (hold < cur ? hold : cur);

    if (cand > cur) {                          /* escalation needs confirmation */
        e->down_count = 0;
        if (e->up_count == 0) { e->pend_level = (uint8_t)cand; }
        else if (cand < e->pend_level) e->pend_level = (uint8_t)cand;
        e->up_count++;
        if (e->up_count >= c->confirm_n)
            set_state(e, e->pend_level == 2 ? EV_CRITICAL : EV_WARNING);
    } else if (cand < cur) {                   /* de-escalation needs confirmation */
        e->up_count = 0;
        e->down_count++;
        if (e->down_count >= c->confirm_n)
            set_state(e, cand == 0 ? EV_RECOVERY : EV_WARNING);
    } else {
        e->up_count = 0; e->down_count = 0;
        if (e->state == EV_RECOVERY) {
            if (++e->rec_count >= c->recover_n) set_state(e, EV_NORMAL);
        }
    }
    return e->state;
}

/* Severity order used to derive the node state. */
int event_severity(uint8_t s)
{
    switch (s) {
    case EV_CRITICAL: return 4;
    case EV_FAULT:    return 3;
    case EV_WARNING:  return 2;
    case EV_RECOVERY: return 1;
    default:          return 0;
    }
}

uint8_t event_node_state(const chan_event_t *ev, int n)
{
    uint8_t s = EV_NORMAL;
    for (int i = 0; i < n; i++)
        if (event_severity(ev[i].state) > event_severity(s)) s = ev[i].state;
    return s;
}

/* sampling.c - adaptive sampling engine.
 *
 *  STABLE   : long interval, environment calm
 *  ACTIVE   : warning, rate-of-change, suspect sample or pending warning
 *  ALERT    : critical state or a critical level waiting for confirmation
 *  RECOVERY : after an event or sensor fault, until stable_confirm calm samples
 */
#include "sampling.h"

void sampler_init(sampler_t *s, uint8_t strategy)
{
    s->mode = (strategy == STRAT_ADAPTIVE) ? SM_ACTIVE : SM_FIXED;
    s->calm_count = 0;
}

uint32_t sampler_interval(const sampling_cfg_t *c, uint8_t mode)
{
    switch (mode) {
    case SM_STABLE:   return c->stable_s;
    case SM_ACTIVE:   return c->active_s;
    case SM_ALERT:    return c->alert_s;
    case SM_RECOVERY: return c->recovery_s;
    default:          return c->fixed_s;
    }
}

uint32_t sampler_next(sampler_t *s, const sampling_cfg_t *c, uint8_t strategy, const sample_cues_t *q)
{
    if (strategy != STRAT_ADAPTIVE) { s->mode = SM_FIXED; return c->fixed_s; }

    if (q->node_state == EV_CRITICAL || q->pending_crit) {
        s->mode = SM_ALERT; s->calm_count = 0;
    } else if (q->node_state == EV_WARNING || q->pending_warn || q->suspect || q->roc_high) {
        s->mode = SM_ACTIVE; s->calm_count = 0;
    } else if (q->node_state == EV_RECOVERY || q->node_state == EV_FAULT) {
        s->mode = SM_RECOVERY; s->calm_count = 0;
    } else if (s->mode != SM_STABLE) {
        /* calm sample: return to the long interval only after confirmation */
        if (++s->calm_count >= c->stable_confirm) { s->mode = SM_STABLE; s->calm_count = 0; }
        else if (s->mode == SM_ALERT) s->mode = SM_RECOVERY;
    }
    return sampler_interval(c, s->mode);
}

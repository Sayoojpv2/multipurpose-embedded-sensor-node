/*
 * event.h - per-channel event state machine with hysteresis and
 * consecutive-sample confirmation.
 *
 *   NORMAL --(confirm_n above warn)--> WARNING --(confirm_n above crit)--> CRITICAL
 *   WARNING/CRITICAL --(confirm_n back inside limits - hyst)--> RECOVERY
 *   RECOVERY --(recover_n normal samples)--> NORMAL
 *   any --(fault_n invalid readings)--> FAULT --(valid reading)--> RECOVERY
 */
#ifndef EVENT_H
#define EVENT_H

#include "profile.h"

typedef struct {
    uint8_t state;        /* event_state_t                          */
    uint8_t pend_level;   /* level waiting for confirmation (0..2)  */
    uint8_t up_count, down_count, rec_count, fault_count;
    uint32_t transitions;
} chan_event_t;

void    event_init(chan_event_t *e);
int     event_level(const thresholds_t *t, float v, float relax);
uint8_t event_update(chan_event_t *e, const thresholds_t *t, const event_cfg_t *c, bool valid, float v);
uint8_t event_node_state(const chan_event_t *ev, int n);
int     event_severity(uint8_t state);
static inline bool event_pending_level(const chan_event_t *e, int lvl) { return e->up_count > 0 && e->pend_level >= lvl; }

#endif

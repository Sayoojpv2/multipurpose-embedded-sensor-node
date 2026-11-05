/*
 * sampling.h - application-aware adaptive sampling engine.
 */
#ifndef SAMPLING_H
#define SAMPLING_H

#include "profile.h"

typedef struct { uint8_t mode; uint8_t calm_count; } sampler_t;

/* Cues extracted from the latest sample. */
typedef struct {
    uint8_t node_state;    /* event_state_t of the node             */
    bool    pending_crit;  /* a channel is confirming a critical level */
    bool    pending_warn;  /* a channel is confirming a warning level  */
    bool    suspect;       /* outlier detector rejected a sample     */
    bool    roc_high;      /* rate of change above profile cue        */
} sample_cues_t;

void     sampler_init(sampler_t *s, uint8_t strategy);
uint32_t sampler_next(sampler_t *s, const sampling_cfg_t *c, uint8_t strategy, const sample_cues_t *q);
uint32_t sampler_interval(const sampling_cfg_t *c, uint8_t mode);

#endif

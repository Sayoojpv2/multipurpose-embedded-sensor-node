/*
 * node.h - sensor manager, telemetry policy, energy model and the node
 * application loop that ties all modules together.
 */
#ifndef NODE_H
#define NODE_H

#include "profile.h"
#include "dsp.h"
#include "event.h"
#include "sampling.h"
#include "packet.h"
#include "comms.h"
#include "storage.h"

/* ---------------- sensor manager ---------------- */
typedef struct {
    const app_profile_t *prof;
    bool     available[MAX_CHANNELS];
    bool     powered[MAX_CHANNELS];
    float    last_valid[MAX_CHANNELS];
    bool     has_last[MAX_CHANNELS];
    uint32_t reads, errors, activations;
} sensor_manager_t;

int  sensor_manager_init(sensor_manager_t *sm, const app_profile_t *p);   /* returns #available */
/* Returns a mask of channels that had to be switched on for this sample. */
uint8_t sensor_manager_sample(sensor_manager_t *sm, uint32_t now, sensor_reading_t *out);
void    sensor_manager_release(sensor_manager_t *sm, uint8_t keep_on_mask);
int  sensor_validate(const sensor_channel_cfg_t *c, float v);

/* ---------------- energy model ---------------- */
typedef struct {
    float v_supply;
    float mcu_active_ma, mcu_idle_ma, sleep_ua;
    float radio_tx_ma, radio_rx_ma;
    float proc_ms_per_sample, proc_ms_per_packet;
    float sensor_switch_uc;     /* charge per sensor power-on transient */
    float battery_mah;
} energy_cfg_t;

typedef struct {
    double q_sensor, q_proc, q_tx, q_rx, q_idle, q_sleep, q_switch;   /* mA*s */
    double t_active_s, t_sleep_s;
} energy_acc_t;

void   energy_default_cfg(energy_cfg_t *c);
void   energy_finalize(energy_acc_t *a, const energy_cfg_t *c, uint32_t duration_s);
double energy_total_mas(const energy_acc_t *a);

/* ---------------- telemetry policy ---------------- */
typedef struct {
    uint32_t last_report_t, last_summary_t, last_alert_t;
    float    last_reported[MAX_CHANNELS];
    bool     has_reported[MAX_CHANNELS];
    float    sum[MAX_CHANNELS], mn[MAX_CHANNELS], mx[MAX_CHANNELS];
    uint16_t cnt[MAX_CHANNELS];
    uint8_t  last_node_state, last_ch_state[MAX_CHANNELS];
    bool     started;
    uint32_t suppressed;   /* samples that produced no transmission */
} telemetry_t;

void telemetry_init(telemetry_t *t);
/* Applies the reporting policy to one sample. Fills *pk (header fields except
 * seq/node/profile) and returns 1 if a packet should be sent, else 0. */
int  telemetry_on_sample(telemetry_t *t, const app_profile_t *p, uint8_t strategy, uint32_t now,
                         const float *filt, const sensor_reading_t *rd, const uint8_t *ch_state,
                         uint8_t node_state, packet_t *pk);

/* ---------------- node ---------------- */
typedef struct {
    uint32_t t, interval;
    uint8_t  mode, node_state, n_tx;
    sensor_reading_t rd[MAX_CHANNELS];
    float    filt[MAX_CHANNELS];
    uint8_t  flags[MAX_CHANNELS], ch_state[MAX_CHANNELS];
} node_sample_log_t;

typedef void (*node_sample_cb)(void *ctx, const node_sample_log_t *s);

typedef struct {
    uint8_t   profile_id, strategy;
    uint16_t  node_id;
    uint32_t  seed;
    uint32_t  override_stable_s;   /* 0 = profile default */
    link_cfg_t link;
    lora_cfg_t lora;
    energy_cfg_t energy;
} node_config_t;

typedef struct {
    const app_profile_t *prof;
    app_profile_t prof_rt;          /* runtime copy (allows overrides) */
    node_config_t cfg;
    sensor_manager_t sm;
    chan_dsp_t   dsp[MAX_CHANNELS];
    chan_event_t ev[MAX_CHANNELS];
    sampler_t    sampler;
    telemetry_t  tel;
    link_t       link;
    storage_t   *storage;
    energy_acc_t e;
    uint16_t     seq;
    uint32_t     last_sample_t, next_t;
    bool         first;
    float        prev_filt[MAX_CHANNELS];
    bool         prev_valid[MAX_CHANNELS];
    uint8_t      fault_reset[MAX_CHANNELS];
    uint32_t     n_samples, n_packets, mode_samples[5];
    node_sample_cb cb;
    void        *cb_ctx;
} node_t;

void     node_default_config(node_config_t *c);
int      node_init(node_t *n, const node_config_t *c, storage_t *st);
uint32_t node_step(node_t *n, uint32_t wake_t); /* one wake-up; returns the next wake-up time */
void     node_finish(node_t *n, uint32_t end_t);
uint32_t node_dsp_mac_ops(const node_t *n);

#endif

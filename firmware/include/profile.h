/*
 * profile.h - application profiles (water / air / soil).
 * A profile is pure configuration data: sensor list, driver selection,
 * calibration, sampling intervals, thresholds, event and telemetry settings.
 */
#ifndef PROFILE_H
#define PROFILE_H

#include "sensor.h"

typedef struct {
    uint32_t fixed_s;      /* conventional strategy period          */
    uint32_t stable_s;     /* adaptive: stable environment          */
    uint32_t active_s;     /* adaptive: moderate change / warning   */
    uint32_t alert_s;      /* adaptive: critical / confirmation     */
    uint32_t recovery_s;   /* adaptive: recovery / sensor fault     */
    uint8_t  stable_confirm; /* calm samples before returning to stable */
} sampling_cfg_t;

typedef struct {
    uint8_t confirm_n;     /* consecutive samples to change level   */
    uint8_t recover_n;     /* consecutive normal samples to leave RECOVERY */
    uint8_t fault_n;       /* consecutive invalid samples -> FAULT  */
} event_cfg_t;

typedef struct {
    uint32_t summary_s;     /* periodic summary when stable          */
    uint32_t min_update_s;  /* rate limit for change-driven updates  */
    uint32_t crit_repeat_s; /* repeat alert while critical           */
} telemetry_cfg_t;

typedef struct {
    uint8_t     id;          /* profile_id_t */
    const char *name;
    const char *description;
    uint8_t     n_channels;
    sensor_channel_cfg_t ch[MAX_CHANNELS];
    sampling_cfg_t  sampling;
    event_cfg_t     event;
    telemetry_cfg_t telemetry;
} app_profile_t;

const app_profile_t *profile_get(uint8_t id);
const app_profile_t *profile_find(const char *name);
int                  profile_count(void);
const app_profile_t *profile_at(int index);

extern const app_profile_t PROFILE_WATER_CFG, PROFILE_AIR_CFG, PROFILE_SOIL_CFG;
extern const float FIR_BINOMIAL7[7];

#endif

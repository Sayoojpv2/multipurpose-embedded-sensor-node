/* sensor_manager.c - registration, availability, acquisition, calibration,
 * validation and fault status for the channels of the active profile. */
#include "node.h"
#include "hal.h"
#include <math.h>
#include <string.h>

int sensor_validate(const sensor_channel_cfg_t *c, float v)
{
    if (isnan(v) || isinf(v)) return RD_OUT_OF_RANGE;
    if (v < c->range_min || v > c->range_max) return RD_OUT_OF_RANGE;
    return RD_OK;
}

int sensor_manager_init(sensor_manager_t *sm, const app_profile_t *p)
{
    memset(sm, 0, sizeof(*sm));
    sm->prof = p;
    int n = 0;
    for (int i = 0; i < p->n_channels; i++) {
        const sensor_channel_cfg_t *c = &p->ch[i];
        if (c->driver == NULL) continue;                 /* not registered */
        hal_sensor_power(c->power_rail, true);
        sm->available[i] = (c->driver->probe == NULL) || (c->driver->probe(c) == 0);
        hal_sensor_power(c->power_rail, false);
        sm->activations++;
        if (sm->available[i]) n++;
    }
    return n;
}

static bool rail_needed(const app_profile_t *p, uint8_t rail, uint8_t mask)
{
    for (int i = 0; i < p->n_channels; i++)
        if ((mask & (1u << i)) && p->ch[i].power_rail == rail) return true;
    return false;
}

uint8_t sensor_manager_sample(sensor_manager_t *sm, uint32_t now, sensor_reading_t *out)
{
    const app_profile_t *p = sm->prof;
    uint8_t switched = 0;

    /* 1. power up every channel that is not already powered */
    for (int i = 0; i < p->n_channels; i++) {
        if (!sm->powered[i]) {
            hal_sensor_power(p->ch[i].power_rail, true);
            sm->powered[i] = true; sm->activations++;
            switched |= (uint8_t)(1u << i);
        }
    }
    /* 2. Wait for every sensor switched on during this wake-up. The shared
     * virtual clock advances by the longest warm-up + conversion interval. */
    float max_on_s = 0.0f;
    for (int i = 0; i < p->n_channels; i++) {
        if (switched & (1u << i)) {
            float on_s = p->ch[i].pwr.warmup_s + p->ch[i].pwr.measure_s;
            if (on_s > max_on_s) max_on_s = on_s;
        }
    }
    if (max_on_s > 0.0f) {
        double wait_ms = ceil((double)max_on_s * 1000.0);
        if (wait_ms > 4294967295.0) wait_ms = 4294967295.0;
        hal_sim_advance_time_ms((uint32_t)wait_ms);
    }
    now = hal_timer_now_s();

    /* 3. bus transactions */
    for (int i = 0; i < p->n_channels; i++) {
        const sensor_channel_cfg_t *c = &p->ch[i];
        sensor_reading_t *r = &out[i];
        r->sensor_id = c->sensor_id; r->type = c->type; r->unit = c->unit;
        r->timestamp = now; r->raw = 0; r->value = NAN;
        if (c->driver == NULL) { r->status = RD_NOT_AVAILABLE; continue; }
        if (!sm->available[i]) {                          /* re-probe: hot-plug / recovered sensor */
            if (c->driver->probe && c->driver->probe(c) != 0) { r->status = RD_NOT_AVAILABLE; continue; }
            sm->available[i] = true;
        }
        r->status = (uint8_t)c->driver->read_raw(c, &r->raw);
        sm->reads++;
    }
    /* 3. conversion: independent channels first, then compensated ones */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < p->n_channels; i++) {
            const sensor_channel_cfg_t *c = &p->ch[i];
            sensor_reading_t *r = &out[i];
            if ((c->comp_channel >= 0) != (pass == 1)) continue;
            if (r->status != RD_OK) continue;
            float aux = NAN;
            if (c->comp_channel >= 0) {
                int k = c->comp_channel;
                if (out[k].status == RD_OK) aux = out[k].value;
                else if (sm->has_last[k]) aux = sm->last_valid[k];
            }
            float v = NAN;
            r->status = (uint8_t)c->driver->convert(c, r->raw, aux, &v);
            if (r->status == RD_OK) {
                v = v * c->cal.gain + c->cal.offset;         /* linear trim */
                r->status = (uint8_t)sensor_validate(c, v);
            }
            r->value = v;
        }
    }
    for (int i = 0; i < p->n_channels; i++) {
        if (out[i].status == RD_OK) { sm->last_valid[i] = out[i].value; sm->has_last[i] = true; }
        else sm->errors++;
    }
    return switched;
}

/* Switch off every channel whose rail is not needed for the next sample. */
void sensor_manager_release(sensor_manager_t *sm, uint8_t keep_on)
{
    const app_profile_t *p = sm->prof;
    for (int i = 0; i < p->n_channels; i++) {
        if (!(keep_on & (1u << i)) && !rail_needed(p, p->ch[i].power_rail, keep_on)) {
            hal_sensor_power(p->ch[i].power_rail, false);
            sm->powered[i] = false;
        }
    }
}

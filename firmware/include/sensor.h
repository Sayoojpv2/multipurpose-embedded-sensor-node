/*
 * sensor.h - modular sensor-driver interface and per-channel configuration.
 *
 * A driver is a small table of function pointers. Adding a new sensor means
 * writing one driver (probe/read_raw/convert) and referencing it from a
 * profile; the sensor manager, DSP, event, telemetry and storage modules do
 * not change.
 */
#ifndef SENSOR_H
#define SENSOR_H

#include "node_types.h"

struct sensor_channel_cfg;

typedef struct sensor_driver {
    const char *name;
    uint8_t     bus;   /* bus_t */
    /* Availability check. Returns 0 if the device answers. */
    int (*probe)(const struct sensor_channel_cfg *cfg);
    /* One bus transaction. Returns read_status_t, writes raw value. */
    int (*read_raw)(const struct sensor_channel_cfg *cfg, int32_t *raw);
    /* Raw -> engineering units using cfg->cal. aux = compensation input
     * (e.g. temperature for EC), NaN if not available. Returns read_status_t. */
    int (*convert)(const struct sensor_channel_cfg *cfg, int32_t raw, float aux, float *out);
} sensor_driver_t;

/* Calibration: driver-specific parameters p[] plus a final linear trim. */
typedef struct { float gain, offset; float p[4]; } calib_t;

/* Demonstration thresholds (NOT regulatory limits). */
typedef struct { float warn_lo, warn_hi, crit_lo, crit_hi, hyst; } thresholds_t;

/* Energy-model parameters for the sensor (assumed, see docs). */
typedef struct { float warmup_s, measure_s, active_ma, off_ua; } sensor_power_t;

typedef struct sensor_channel_cfg {
    uint8_t  sensor_id;
    uint8_t  type;           /* sensor_type_t */
    uint8_t  unit;           /* unit_t        */
    const char *name;
    const char *unit_str;
    const sensor_driver_t *driver;
    uint8_t  bus_addr;       /* ADC channel / I2C addr / SPI CS / UART port */
    uint8_t  bus_sub;        /* sub-channel: ADS mux input, SHT field ...    */
    uint8_t  power_rail;
    calib_t  cal;
    float    range_min, range_max;   /* plausibility check               */
    float    pkt_scale;              /* encoded int16 = value * scale    */
    int8_t   comp_channel;           /* channel index used as aux, -1    */
    /* signal processing */
    uint8_t  ma_len;                 /* 1 = moving average disabled      */
    const float *fir_coeffs;         /* NULL = FIR disabled              */
    uint8_t  fir_taps;
    uint8_t  hampel_on;
    float    hampel_k, hampel_min_dev;
    /* events and telemetry */
    thresholds_t thr;
    float    delta_report;           /* change that triggers an UPDATE   */
    float    roc_per_min;            /* rate-of-change cue for sampling  */
    sensor_power_t pwr;
} sensor_channel_cfg_t;

/* Driver lookup by name (used for diagnostics / configuration). */
const sensor_driver_t *sensor_driver_find(const char *name);

/* Exported driver objects (firmware/drivers). */
extern const sensor_driver_t DRV_PH_ANALOG, DRV_TURBIDITY_ANALOG, DRV_MOISTURE_ANALOG,
    DRV_NTC_ANALOG, DRV_EC_ADS_I2C, DRV_RTD_SPI, DRV_PM_UART, DRV_CO2_I2C,
    DRV_SHT_I2C, DRV_LIGHT_I2C;

#endif

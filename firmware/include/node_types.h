/*
 * node_types.h - common types shared by every firmware module.
 * Pure C99, no dynamic memory, no OS. Portable to native host and Cortex-M.
 */
#ifndef NODE_TYPES_H
#define NODE_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define FW_VERSION_STR   "1.0.0"
#define MAX_CHANNELS     4      /* sensor channels per application profile   */
#define MA_MAX_LEN       8      /* longest moving-average window              */
#define FIR_MAX_TAPS     15     /* longest FIR filter                         */
#define HAMPEL_WIN       5      /* outlier detector window (odd)              */

/* Value used in thresholds to mean "no limit on this side". */
#define THR_NONE_HI      1.0e9f
#define THR_NONE_LO     -1.0e9f

typedef enum { PROFILE_WATER = 1, PROFILE_AIR = 2, PROFILE_SOIL = 3 } profile_id_t;

typedef enum {
    SENSOR_PH = 1, SENSOR_TURBIDITY, SENSOR_EC_WATER, SENSOR_TEMP_WATER,
    SENSOR_PM25, SENSOR_CO2, SENSOR_RH, SENSOR_TEMP_AIR,
    SENSOR_SOIL_MOISTURE, SENSOR_SOIL_TEMP, SENSOR_SOIL_EC, SENSOR_LIGHT
} sensor_type_t;

typedef enum {
    UNIT_PH = 0, UNIT_NTU, UNIT_US_CM, UNIT_DEG_C, UNIT_UG_M3,
    UNIT_PPM, UNIT_PCT_RH, UNIT_PCT_VWC, UNIT_LUX
} unit_t;

typedef enum { BUS_ADC = 0, BUS_I2C, BUS_SPI, BUS_UART } bus_t;

/* Result of one acquisition attempt (driver + validation). */
typedef enum {
    RD_OK = 0,
    RD_NO_RESPONSE,     /* I2C NACK, UART timeout, SPI MISO stuck high     */
    RD_OUT_OF_RANGE,    /* converted value outside plausible physical range */
    RD_RAIL,            /* ADC at supply rail / full-scale: open or short   */
    RD_CHECKSUM,        /* frame or CRC-8 check failed                      */
    RD_NOT_AVAILABLE    /* channel failed registration/probe                */
} read_status_t;

/* The consistent internal sensor data structure. */
typedef struct {
    uint8_t  sensor_id;   /* unique id inside the profile (packet field)    */
    uint8_t  type;        /* sensor_type_t                                  */
    uint8_t  unit;        /* unit_t                                         */
    uint8_t  status;      /* read_status_t                                  */
    int32_t  raw;         /* raw bus value (ADC counts, register word ...)  */
    float    value;       /* calibrated value in engineering units          */
    uint32_t timestamp;   /* node time in seconds                           */
} sensor_reading_t;

static inline bool reading_valid(const sensor_reading_t *r) { return r->status == RD_OK; }

/* Per-channel and node-level event state. */
typedef enum { EV_NORMAL = 0, EV_WARNING = 1, EV_CRITICAL = 2, EV_RECOVERY = 3, EV_FAULT = 4 } event_state_t;

/* Telemetry priority, 2 bits in the packet. */
typedef enum { PRIO_LOW = 0, PRIO_NORMAL = 1, PRIO_HIGH = 2, PRIO_CRITICAL = 3 } priority_t;

/* Communication strategies compared in the experiments. */
typedef enum {
    STRAT_FIXED = 0,        /* conventional fixed-period sampling + reporting */
    STRAT_ADAPTIVE_TX = 1,  /* ablation: fixed sampling, adaptive telemetry   */
    STRAT_ADAPTIVE = 2      /* proposed: adaptive sampling + adaptive telemetry */
} strategy_t;

/* Sampling-engine modes. */
typedef enum { SM_FIXED = 0, SM_STABLE, SM_ACTIVE, SM_ALERT, SM_RECOVERY } sample_mode_t;

#endif

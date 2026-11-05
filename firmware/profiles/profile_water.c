/* profile_water.c - Profile A: water-quality monitoring.
 * Thresholds are DEMONSTRATION values for testing the firmware logic,
 * not regulatory limits. */
#include "profile.h"
#include "hal.h"

const app_profile_t PROFILE_WATER_CFG = {
    .id = PROFILE_WATER, .name = "water",
    .description = "Water monitoring: pH, turbidity, electrical conductivity, temperature",
    .n_channels = 4,
    .ch = {
        { .sensor_id = 1, .type = SENSOR_PH, .unit = UNIT_PH, .name = "pH", .unit_str = "pH",
          .driver = &DRV_PH_ANALOG, .bus_addr = 0, .power_rail = 0,
          .cal = {1.0f, 0.0f, {1.680f, 0.17393f, 0, 0}},   /* Vmid, slope from 2-point buffer cal */
          .range_min = 0.0f, .range_max = 14.0f, .pkt_scale = 100.0f, .comp_channel = -1,
          .ma_len = 4, .hampel_on = 1, .hampel_k = 3.0f, .hampel_min_dev = 0.15f,
          .thr = {6.5f, 8.5f, 6.0f, 9.0f, 0.10f}, .delta_report = 0.2f, .roc_per_min = 0.05f,
          .pwr = {2.0f, 0.1f, 1.5f, 0.1f} },
        { .sensor_id = 2, .type = SENSOR_TURBIDITY, .unit = UNIT_NTU, .name = "turbidity", .unit_str = "NTU",
          .driver = &DRV_TURBIDITY_ANALOG, .bus_addr = 1, .power_rail = 1,
          .cal = {1.0f, 0.0f, {3.0f, 0.0025f, 0, 0}},
          .range_min = 0.0f, .range_max = 1000.0f, .pkt_scale = 10.0f, .comp_channel = -1,
          .ma_len = 4, .hampel_on = 1, .hampel_k = 3.0f, .hampel_min_dev = 5.0f,
          .thr = {THR_NONE_LO, 50.0f, THR_NONE_LO, 100.0f, 5.0f}, .delta_report = 10.0f, .roc_per_min = 2.0f,
          .pwr = {0.5f, 0.1f, 30.0f, 0.1f} },
        { .sensor_id = 3, .type = SENSOR_EC_WATER, .unit = UNIT_US_CM, .name = "conductivity", .unit_str = "uS/cm",
          .driver = &DRV_EC_ADS_I2C, .bus_addr = 0x48, .bus_sub = 0, .power_rail = 2,
          .cal = {1.0f, 0.0f, {0.001f, 0.02f, 0, 0}},      /* V per uS/cm, temperature coefficient */
          .range_min = 0.0f, .range_max = 3000.0f, .pkt_scale = 1.0f, .comp_channel = 3,
          .ma_len = 4, .hampel_on = 1, .hampel_k = 3.0f, .hampel_min_dev = 20.0f,
          .thr = {THR_NONE_LO, 1000.0f, THR_NONE_LO, 1500.0f, 30.0f}, .delta_report = 50.0f, .roc_per_min = 10.0f,
          .pwr = {1.0f, 0.2f, 3.0f, 0.1f} },
        { .sensor_id = 4, .type = SENSOR_TEMP_WATER, .unit = UNIT_DEG_C, .name = "water_temp", .unit_str = "degC",
          .driver = &DRV_RTD_SPI, .bus_addr = 0, .power_rail = 2,
          .cal = {1.0f, 0.0f, {100.0f, 430.0f, 0, 0}},     /* PT100 R0, reference resistor */
          .range_min = -20.0f, .range_max = 80.0f, .pkt_scale = 100.0f, .comp_channel = -1,
          .ma_len = 3, .hampel_on = 1, .hampel_k = 3.0f, .hampel_min_dev = 0.5f,
          .thr = {15.0f, 32.0f, 10.0f, 35.0f, 0.5f}, .delta_report = 1.0f, .roc_per_min = 0.2f,
          .pwr = {0.1f, 0.07f, 2.0f, 0.1f} },
    },
    .sampling  = {.fixed_s = 60, .stable_s = 300, .active_s = 60, .alert_s = 15, .recovery_s = 60, .stable_confirm = 5},
    .event     = {.confirm_n = 2, .recover_n = 5, .fault_n = 3},
    .telemetry = {.summary_s = 900, .min_update_s = 60, .crit_repeat_s = 120},
};

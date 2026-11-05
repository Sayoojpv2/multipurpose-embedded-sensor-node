/* profile_soil.c - Profile C: soil / agriculture monitoring.
 * Thresholds are DEMONSTRATION values, not agronomic recommendations. */
#include "profile.h"
#include "hal.h"

const app_profile_t PROFILE_SOIL_CFG = {
    .id = PROFILE_SOIL, .name = "soil",
    .description = "Soil monitoring: soil moisture, soil temperature, soil EC, light intensity",
    .n_channels = 4,
    .ch = {
        { .sensor_id = 1, .type = SENSOR_SOIL_MOISTURE, .unit = UNIT_PCT_VWC, .name = "soil_moisture", .unit_str = "%VWC",
          .driver = &DRV_MOISTURE_ANALOG, .bus_addr = 0, .power_rail = 0,
          .cal = {1.0f, 0.0f, {2.8f, 1.3f, 50.0f, 0}},       /* V_dry, V_wet, VWC at wet point */
          .range_min = 0.0f, .range_max = 60.0f, .pkt_scale = 100.0f, .comp_channel = -1,
          .ma_len = 4, .hampel_on = 1, .hampel_k = 3.0f, .hampel_min_dev = 2.0f,
          .thr = {20.0f, 45.0f, 12.0f, 50.0f, 1.0f}, .delta_report = 3.0f, .roc_per_min = 0.5f,
          .pwr = {0.5f, 0.1f, 5.0f, 0.1f} },
        { .sensor_id = 2, .type = SENSOR_SOIL_TEMP, .unit = UNIT_DEG_C, .name = "soil_temp", .unit_str = "degC",
          .driver = &DRV_NTC_ANALOG, .bus_addr = 1, .power_rail = 1,
          .cal = {1.0f, 0.0f, {10000.0f, 10000.0f, 298.15f, 3950.0f}},  /* R0, Rseries, T0, Beta */
          .range_min = -20.0f, .range_max = 60.0f, .pkt_scale = 100.0f, .comp_channel = -1,
          .ma_len = 4, .hampel_on = 1, .hampel_k = 3.0f, .hampel_min_dev = 0.5f,
          .thr = {5.0f, 35.0f, 2.0f, 40.0f, 0.5f}, .delta_report = 1.0f, .roc_per_min = 0.2f,
          .pwr = {0.01f, 0.01f, 0.17f, 0.0f} },
        { .sensor_id = 3, .type = SENSOR_SOIL_EC, .unit = UNIT_US_CM, .name = "soil_ec", .unit_str = "uS/cm",
          .driver = &DRV_EC_ADS_I2C, .bus_addr = 0x48, .bus_sub = 1, .power_rail = 2,
          .cal = {1.0f, 0.0f, {0.0005f, 0.02f, 0, 0}},     /* same driver as water EC, other calibration */
          .range_min = 0.0f, .range_max = 6000.0f, .pkt_scale = 1.0f, .comp_channel = 1,
          .ma_len = 4, .hampel_on = 1, .hampel_k = 3.0f, .hampel_min_dev = 100.0f,
          .thr = {THR_NONE_LO, 2500.0f, THR_NONE_LO, 4000.0f, 100.0f}, .delta_report = 200.0f, .roc_per_min = 50.0f,
          .pwr = {1.0f, 0.2f, 3.0f, 0.1f} },
        { .sensor_id = 4, .type = SENSOR_LIGHT, .unit = UNIT_LUX, .name = "light", .unit_str = "lux",
          .driver = &DRV_LIGHT_I2C, .bus_addr = 0x23, .power_rail = 3,
          .cal = {1.0f, 0.0f, {0, 0, 0, 0}},
          .range_min = 0.0f, .range_max = 54613.0f, .pkt_scale = 0.5f, .comp_channel = -1,
          .ma_len = 1, .hampel_on = 0,
          .thr = {THR_NONE_LO, THR_NONE_HI, THR_NONE_LO, THR_NONE_HI, 0.0f},  /* informational channel */
          .delta_report = 5000.0f, .roc_per_min = 1.0e9f,
          .pwr = {0.0f, 0.18f, 0.12f, 0.01f} },
    },
    .sampling  = {.fixed_s = 300, .stable_s = 1800, .active_s = 300, .alert_s = 60, .recovery_s = 300, .stable_confirm = 4},
    .event     = {.confirm_n = 2, .recover_n = 4, .fault_n = 3},
    .telemetry = {.summary_s = 3600, .min_update_s = 300, .crit_repeat_s = 600},
};

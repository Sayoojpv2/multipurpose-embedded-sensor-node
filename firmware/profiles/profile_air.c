/* profile_air.c - Profile B: air / environment monitoring.
 * Thresholds are DEMONSTRATION values, not regulatory limits. */
#include "profile.h"
#include "hal.h"

const app_profile_t PROFILE_AIR_CFG = {
    .id = PROFILE_AIR, .name = "air",
    .description = "Air monitoring: PM2.5, CO2, relative humidity, temperature",
    .n_channels = 4,
    .ch = {
        { .sensor_id = 1, .type = SENSOR_PM25, .unit = UNIT_UG_M3, .name = "pm2_5", .unit_str = "ug/m3",
          .driver = &DRV_PM_UART, .bus_addr = HAL_UART_SENSOR, .power_rail = 0,
          .cal = {1.0f, 0.0f, {0, 0, 0, 0}},
          .range_min = 0.0f, .range_max = 1000.0f, .pkt_scale = 10.0f, .comp_channel = -1,
          .ma_len = 1, .fir_coeffs = FIR_BINOMIAL7, .fir_taps = 7,      /* FIR instead of MA */
          .hampel_on = 1, .hampel_k = 3.0f, .hampel_min_dev = 15.0f,
          .thr = {THR_NONE_LO, 60.0f, THR_NONE_LO, 120.0f, 5.0f}, .delta_report = 10.0f, .roc_per_min = 5.0f,
          .pwr = {30.0f, 1.0f, 80.0f, 0.5f} },                 /* laser PM sensor: fan warm-up */
        { .sensor_id = 2, .type = SENSOR_CO2, .unit = UNIT_PPM, .name = "co2", .unit_str = "ppm",
          .driver = &DRV_CO2_I2C, .bus_addr = 0x62, .power_rail = 1,
          .cal = {1.0f, 0.0f, {0, 0, 0, 0}},
          .range_min = 400.0f, .range_max = 5000.0f, .pkt_scale = 1.0f, .comp_channel = -1,
          .ma_len = 3, .hampel_on = 1, .hampel_k = 3.0f, .hampel_min_dev = 50.0f,
          .thr = {THR_NONE_LO, 1200.0f, THR_NONE_LO, 2000.0f, 50.0f}, .delta_report = 100.0f, .roc_per_min = 50.0f,
          .pwr = {0.05f, 5.0f, 15.0f, 0.5f} },
        { .sensor_id = 3, .type = SENSOR_RH, .unit = UNIT_PCT_RH, .name = "humidity", .unit_str = "%RH",
          .driver = &DRV_SHT_I2C, .bus_addr = 0x44, .bus_sub = 1, .power_rail = 2,
          .cal = {1.0f, 0.0f, {0, 0, 0, 0}},
          .range_min = 0.0f, .range_max = 100.0f, .pkt_scale = 100.0f, .comp_channel = -1,
          .ma_len = 3, .hampel_on = 1, .hampel_k = 3.0f, .hampel_min_dev = 3.0f,
          .thr = {25.0f, 85.0f, 15.0f, 95.0f, 2.0f}, .delta_report = 5.0f, .roc_per_min = 2.0f,
          .pwr = {0.001f, 0.015f, 0.6f, 0.1f} },
        { .sensor_id = 4, .type = SENSOR_TEMP_AIR, .unit = UNIT_DEG_C, .name = "air_temp", .unit_str = "degC",
          .driver = &DRV_SHT_I2C, .bus_addr = 0x44, .bus_sub = 0, .power_rail = 2,
          .cal = {1.0f, 0.0f, {0, 0, 0, 0}},
          .range_min = -40.0f, .range_max = 85.0f, .pkt_scale = 100.0f, .comp_channel = -1,
          .ma_len = 3, .hampel_on = 1, .hampel_k = 3.0f, .hampel_min_dev = 0.5f,
          .thr = {THR_NONE_LO, 38.0f, THR_NONE_LO, 45.0f, 0.5f}, .delta_report = 1.0f, .roc_per_min = 0.3f,
          .pwr = {0.001f, 0.015f, 0.6f, 0.1f} },
    },
    .sampling  = {.fixed_s = 60, .stable_s = 180, .active_s = 60, .alert_s = 30, .recovery_s = 60, .stable_confirm = 5},
    .event     = {.confirm_n = 2, .recover_n = 5, .fault_n = 3},
    .telemetry = {.summary_s = 600, .min_update_s = 60, .crit_repeat_s = 60},
};

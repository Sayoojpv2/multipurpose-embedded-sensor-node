/* energy.c - configurable charge/energy model (software ESTIMATES only).
 *
 * Radio values: RAK3172 datasheet (RAKwireless), TX 87 mA @ 20 dBm 868 MHz,
 * RX 5.22 mA, sleep 1.69 uA typ. @ 3.3 V.
 *   https://docs.rakwireless.com/product-categories/wisduo/rak3172-module/datasheet/
 * MCU active/idle currents, processing times and sensor currents are
 * ASSUMPTIONS documented in docs/technical_report.md - change them here.
 */
#include "node.h"

void energy_default_cfg(energy_cfg_t *c)
{
    c->v_supply = 3.3f;
    c->radio_tx_ma = 87.0f;       /* datasheet */
    c->radio_rx_ma = 5.22f;       /* datasheet */
    c->sleep_ua = 1.69f;          /* datasheet, module sleep */
    c->mcu_active_ma = 4.0f;      /* assumption: Cortex-M4 class running at ~48 MHz */
    c->mcu_idle_ma = 1.0f;        /* assumption: MCU sleep-with-peripherals while sensors convert */
    c->proc_ms_per_sample = 3.0f; /* assumption: acquisition + DSP + events + storage */
    c->proc_ms_per_packet = 2.0f; /* assumption: encode + CRC + radio driver */
    c->sensor_switch_uc = 20.0f;  /* assumption: load-switch / inrush charge per power-up */
    c->battery_mah = 2600.0f;     /* assumption: battery capacity used for lifetime projection */
}

void energy_finalize(energy_acc_t *a, const energy_cfg_t *c, uint32_t duration_s)
{
    a->t_sleep_s = (double)duration_s - a->t_active_s;
    if (a->t_sleep_s < 0) a->t_sleep_s = 0;
    a->q_sleep = a->t_sleep_s * c->sleep_ua / 1000.0;
}

double energy_total_mas(const energy_acc_t *a)
{
    return a->q_sensor + a->q_proc + a->q_tx + a->q_rx + a->q_idle + a->q_sleep + a->q_switch;
}

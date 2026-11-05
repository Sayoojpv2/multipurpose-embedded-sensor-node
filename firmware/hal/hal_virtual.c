/* hal_virtual.c - HAL implementation backed by virtual sensor devices.
 * Time advances explicitly for sensor warm-up; it is model time, not measured
 * processor execution time. */
#include "hal.h"
#include "../../simulation/sensors/virtual_sensors.h"

static uint64_t g_now_ms;
static uint32_t g_wakeup;
static uint16_t g_gpio;

int hal_adc_read(uint8_t ch, uint16_t *raw) { return vdev_adc(ch, raw); }
int hal_i2c_write(uint8_t a, const uint8_t *d, size_t n) { return vdev_i2c_write(a, d, n); }
int hal_i2c_read(uint8_t a, uint8_t *d, size_t n) { return vdev_i2c_read(a, d, n); }
int hal_spi_transfer(uint8_t cs, const uint8_t *tx, uint8_t *rx, size_t n) { return vdev_spi(cs, tx, rx, n); }

int hal_uart_write(uint8_t port, const uint8_t *d, size_t n)
{
    if (port == HAL_UART_CONSOLE) { target_console_write(d, n); return (int)n; }
    return HAL_ERR_PARAM;
}

int hal_uart_read(uint8_t port, uint8_t *d, size_t n, uint32_t timeout_ms)
{
    (void)timeout_ms;
    return vdev_uart_read(port, d, n);
}

uint32_t hal_timer_now_s(void) { return (uint32_t)(g_now_ms / 1000u); }
void hal_timer_set_wakeup(uint32_t t) { g_wakeup = t; }
uint32_t hal_timer_get_wakeup(void) { return g_wakeup; }
void hal_sim_set_time(uint32_t t) { g_now_ms = (uint64_t)t * 1000u; }
void hal_sim_advance_time_ms(uint32_t milliseconds) { g_now_ms += milliseconds; }

void hal_gpio_write(uint8_t pin, bool level)
{
    if (pin >= 16) return;
    if (level) g_gpio |= (uint16_t)(1u << pin); else g_gpio &= (uint16_t)~(1u << pin);
    if (pin >= HAL_PIN_RAIL_BASE) vdev_set_rail((uint8_t)(pin - HAL_PIN_RAIL_BASE), level);
}

bool hal_gpio_read(uint8_t pin) { return pin < 16 && (g_gpio >> pin) & 1u; }

void hal_sensor_power(uint8_t rail, bool on) { hal_gpio_write((uint8_t)(HAL_PIN_RAIL_BASE + rail), on); }

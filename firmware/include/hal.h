/*
 * hal.h - Hardware Abstraction Layer.
 *
 * The firmware only talks to hardware through these functions. In this
 * project they are implemented by hal/hal_virtual.c, which routes bus
 * transactions to *virtual* sensor devices (simulation/sensors). No physical
 * hardware is connected. A real port would re-implement this file against a
 * vendor HAL (e.g. STM32 LL/HAL) without touching drivers or application code.
 */
#ifndef HAL_H
#define HAL_H

#include "node_types.h"

typedef enum { HAL_OK = 0, HAL_ERR_NACK = -1, HAL_ERR_TIMEOUT = -2, HAL_ERR_PARAM = -3 } hal_status_t;

#define HAL_ADC_BITS   12
#define HAL_ADC_MAX    4095u
#define HAL_ADC_VREF   3.3f

#define HAL_UART_CONSOLE 0
#define HAL_UART_SENSOR  1

#define HAL_PIN_RAIL_BASE 8   /* GPIO pins 8..15 drive sensor load switches */

/* ADC: single conversion, 12-bit right aligned. */
int  hal_adc_read(uint8_t channel, uint16_t *raw);

/* I2C master, 7-bit address. */
int  hal_i2c_write(uint8_t addr7, const uint8_t *data, size_t len);
int  hal_i2c_read(uint8_t addr7, uint8_t *data, size_t len);

/* SPI full-duplex transfer with chip-select number cs. */
int  hal_spi_transfer(uint8_t cs, const uint8_t *tx, uint8_t *rx, size_t len);

/* UART. read returns number of bytes received before timeout (>=0). */
int  hal_uart_write(uint8_t port, const uint8_t *data, size_t len);
int  hal_uart_read(uint8_t port, uint8_t *data, size_t len, uint32_t timeout_ms);

/* Timer / RTC (1 s resolution node time). */
uint32_t hal_timer_now_s(void);
void     hal_timer_set_wakeup(uint32_t t_s);
uint32_t hal_timer_get_wakeup(void);

/* GPIO and sensor power control (load switch per power rail). */
void hal_gpio_write(uint8_t pin, bool level);
bool hal_gpio_read(uint8_t pin);
void hal_sensor_power(uint8_t rail, bool on);

/* Simulation-side control (implemented by hal_virtual.c). */
void hal_sim_set_time(uint32_t t_s);

/* Provided by the build target: native -> stdout, ARM -> USART register. */
void target_console_write(const uint8_t *data, size_t len);

#endif

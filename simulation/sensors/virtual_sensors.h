/*
 * virtual_sensors.h - VIRTUAL sensor backend (simulation only).
 *
 * The environment provides physical quantities from a trace (CSV on the host,
 * const arrays on the ARM image). Virtual devices convert those quantities
 * into what a real part would put on the bus (ADC counts, I2C registers, SPI
 * bytes, UART frames) using their own inverse transfer functions. The
 * firmware drivers then have to decode them exactly as with real hardware.
 */
#ifndef VIRTUAL_SENSORS_H
#define VIRTUAL_SENSORS_H

#include "node_types.h"

typedef struct {
    uint32_t n_rows, dt_s;
    const float   *v[MAX_CHANNELS];      /* measured physical value per channel   */
    const uint8_t *fault[MAX_CHANNELS];  /* 0 ok, 1 no response/missing, 2 stuck   */
    const uint8_t *link;                 /* 1 = radio link available               */
} env_trace_t;

void     env_set_trace(const env_trace_t *t);
uint32_t env_duration_s(void);
int      env_sample(int ch, uint32_t t, float *v);   /* returns fault code */
bool     env_link_up(uint32_t t);

/* Install the virtual "board" (which device sits on which bus) for a profile. */
int  vdev_load_board(uint8_t profile_id);
void vdev_set_rail(uint8_t rail, bool on);
int  vdev_adc(uint8_t ch, uint16_t *raw);
int  vdev_i2c_write(uint8_t addr, const uint8_t *d, size_t len);
int  vdev_i2c_read(uint8_t addr, uint8_t *d, size_t len);
int  vdev_spi(uint8_t cs, const uint8_t *tx, uint8_t *rx, size_t len);
int  vdev_uart_read(uint8_t port, uint8_t *d, size_t len);

#endif

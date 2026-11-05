/* digital_drivers.c - I2C, SPI and UART sensor drivers.
 * Register maps / frame layouts are modelled on common sensor families
 * (ADS1115-style ADC, MAX31865-style RTD converter, Plantower PMS-style UART
 * frame, Sensirion SCD4x/SHT3x-style words with CRC-8, BH1750-style light).
 * They are "style" models written for this project, not vendor code. */
#include "sensor.h"
#include "hal.h"
#include "comms.h"
#include <math.h>
#include <string.h>

static int i2c_probe(const sensor_channel_cfg_t *c)
{
    uint8_t b;
    return hal_i2c_read(c->bus_addr, &b, 1) == HAL_OK ? 0 : -1;
}

/* ---------- ADS1115-style 16-bit I2C ADC used for EC (water and soil) ----------
 * config: OS=1 | MUX=AINx vs GND | PGA=+-4.096 V | single-shot | 128 SPS | comparator off
 * conversion register 0x00, big-endian signed 16-bit, LSB = 125 uV */
static int ec_read(const sensor_channel_cfg_t *c, int32_t *raw)
{
    uint16_t cfg = (uint16_t)(0x8000u | ((4u + c->bus_sub) << 12) | (1u << 9) | (1u << 8) | (4u << 5) | 0x03u);
    uint8_t w[3] = {0x01, (uint8_t)(cfg >> 8), (uint8_t)cfg};
    uint8_t ptr = 0x00, rx[2];
    if (hal_i2c_write(c->bus_addr, w, 3) != HAL_OK) return RD_NO_RESPONSE;
    if (hal_i2c_write(c->bus_addr, &ptr, 1) != HAL_OK) return RD_NO_RESPONSE;
    if (hal_i2c_read(c->bus_addr, rx, 2) != HAL_OK) return RD_NO_RESPONSE;
    int16_t code = (int16_t)((rx[0] << 8) | rx[1]);
    *raw = code;
    if (code >= 32767 || code <= -32768) return RD_RAIL;
    return RD_OK;
}

/* EC probe front-end: V = EC_T * p0. Temperature compensation to 25 C:
 * EC25 = EC_T / (1 + p1*(T - 25)), T from the compensation channel (aux). */
static int ec_convert(const sensor_channel_cfg_t *c, int32_t raw, float aux, float *out)
{
    float v = (float)raw * 4.096f / 32768.0f;
    float ec_t = v / c->cal.p[0];
    float t = isnan(aux) ? 25.0f : aux;          /* no temperature: no compensation */
    *out = ec_t / (1.0f + c->cal.p[1] * (t - 25.0f));
    return RD_OK;
}

/* ---------- MAX31865-style RTD-to-digital over SPI (PT100) ----------
 * read reg 0x01..0x02: 15-bit ratio code (R/Rref*32768) << 1 | fault bit */
static int rtd_probe(const sensor_channel_cfg_t *c)
{
    uint8_t tx[2] = {0x00, 0x00}, rx[2];
    hal_spi_transfer(c->bus_addr, tx, rx, 2);
    return rx[1] == 0xFF ? -1 : 0;
}

static int rtd_read(const sensor_channel_cfg_t *c, int32_t *raw)
{
    uint8_t tx[3] = {0x01, 0, 0}, rx[3];
    if (hal_spi_transfer(c->bus_addr, tx, rx, 3) != HAL_OK) return RD_NO_RESPONSE;
    uint16_t code = (uint16_t)((rx[1] << 8) | rx[2]);
    if (code == 0xFFFF) return RD_NO_RESPONSE;       /* MISO pulled high: no device */
    *raw = code >> 1;
    if (code & 1u) return RD_RAIL;                   /* converter fault bit (open RTD) */
    return RD_OK;
}

/* Callendar-Van Dusen (T >= 0 C), solved for T:  R = R0(1 + A T + B T^2)
 * p0 = R0 (100 ohm), p1 = Rref (430 ohm) */
static int rtd_convert(const sensor_channel_cfg_t *c, int32_t raw, float aux, float *out)
{
    (void)aux;
    const float A = 3.9083e-3f, B = -5.775e-7f;
    float r = (float)raw * c->cal.p[1] / 32768.0f;
    float disc = A * A - 4.0f * B * (1.0f - r / c->cal.p[0]);
    if (disc < 0.0f) return RD_OUT_OF_RANGE;
    *out = (-A + sqrtf(disc)) / (2.0f * B);
    return RD_OK;
}

/* ---------- PMS-style 32-byte UART frame ----------
 * 0x42 0x4D len(2) 13 data words, checksum(2) = sum of bytes 0..29.
 * data word 5 (bytes 12-13) = PM2.5 [ug/m3] atmospheric environment */
static int pm_probe(const sensor_channel_cfg_t *c) { (void)c; return 0; }  /* stream device: checked on read */

static int pm_read(const sensor_channel_cfg_t *c, int32_t *raw)
{
    uint8_t f[32];
    int n = hal_uart_read(c->bus_addr, f, sizeof f, 1000);
    if (n < (int)sizeof f) return RD_NO_RESPONSE;
    if (f[0] != 0x42 || f[1] != 0x4D) return RD_CHECKSUM;
    uint16_t sum = 0;
    for (int i = 0; i < 30; i++) sum = (uint16_t)(sum + f[i]);
    if (sum != (uint16_t)((f[30] << 8) | f[31])) return RD_CHECKSUM;
    *raw = (f[12] << 8) | f[13];
    return RD_OK;
}

static int direct_convert(const sensor_channel_cfg_t *c, int32_t raw, float aux, float *out)
{
    (void)c; (void)aux;
    *out = (float)raw;
    return RD_OK;
}

/* ---------- Sensirion-style words: [MSB LSB CRC8] ---------- */
static bool word_ok(const uint8_t *w) { return crc8_sensirion(w, 2) == w[2]; }

/* SCD4x-style CO2: single-shot command 0x219D, read_measurement 0xEC05 -> 9 bytes */
static int co2_read(const sensor_channel_cfg_t *c, int32_t *raw)
{
    uint8_t meas[2] = {0x21, 0x9D}, rd[2] = {0xEC, 0x05}, rx[9];
    if (hal_i2c_write(c->bus_addr, meas, 2) != HAL_OK) return RD_NO_RESPONSE;
    if (hal_i2c_write(c->bus_addr, rd, 2) != HAL_OK) return RD_NO_RESPONSE;
    if (hal_i2c_read(c->bus_addr, rx, 9) != HAL_OK) return RD_NO_RESPONSE;
    if (!word_ok(&rx[0])) return RD_CHECKSUM;
    *raw = (rx[0] << 8) | rx[1];
    return RD_OK;
}

/* SHT3x-style T/RH: command 0x2400 -> T word + CRC, RH word + CRC.
 * bus_sub 0 = temperature, 1 = relative humidity */
static int sht_read(const sensor_channel_cfg_t *c, int32_t *raw)
{
    uint8_t cmd[2] = {0x24, 0x00}, rx[6];
    if (hal_i2c_write(c->bus_addr, cmd, 2) != HAL_OK) return RD_NO_RESPONSE;
    if (hal_i2c_read(c->bus_addr, rx, 6) != HAL_OK) return RD_NO_RESPONSE;
    const uint8_t *w = &rx[c->bus_sub ? 3 : 0];
    if (!word_ok(w)) return RD_CHECKSUM;
    *raw = (w[0] << 8) | w[1];
    return RD_OK;
}

static int sht_convert(const sensor_channel_cfg_t *c, int32_t raw, float aux, float *out)
{
    (void)aux;
    *out = c->bus_sub ? 100.0f * (float)raw / 65535.0f : -45.0f + 175.0f * (float)raw / 65535.0f;
    return RD_OK;
}

/* BH1750-style light: one-time high-res 0x20, 2 bytes, lux = raw / 1.2 */
static int light_read(const sensor_channel_cfg_t *c, int32_t *raw)
{
    uint8_t cmd = 0x20, rx[2];
    if (hal_i2c_write(c->bus_addr, &cmd, 1) != HAL_OK) return RD_NO_RESPONSE;
    if (hal_i2c_read(c->bus_addr, rx, 2) != HAL_OK) return RD_NO_RESPONSE;
    *raw = (rx[0] << 8) | rx[1];
    if (*raw == 0xFFFF) return RD_RAIL;              /* saturated */
    return RD_OK;
}

static int light_convert(const sensor_channel_cfg_t *c, int32_t raw, float aux, float *out)
{
    (void)c; (void)aux;
    *out = (float)raw / 1.2f;
    return RD_OK;
}

const sensor_driver_t DRV_EC_ADS_I2C = {"ec_ads_i2c", BUS_I2C,  i2c_probe, ec_read,    ec_convert};
const sensor_driver_t DRV_RTD_SPI    = {"rtd_spi",    BUS_SPI,  rtd_probe, rtd_read,   rtd_convert};
const sensor_driver_t DRV_PM_UART    = {"pm_uart",    BUS_UART, pm_probe,  pm_read,    direct_convert};
const sensor_driver_t DRV_CO2_I2C    = {"co2_i2c",    BUS_I2C,  i2c_probe, co2_read,   direct_convert};
const sensor_driver_t DRV_SHT_I2C    = {"sht_i2c",    BUS_I2C,  i2c_probe, sht_read,   sht_convert};
const sensor_driver_t DRV_LIGHT_I2C  = {"light_i2c",  BUS_I2C,  i2c_probe, light_read, light_convert};

static const sensor_driver_t *const ALL_DRIVERS[] = {
    &DRV_PH_ANALOG, &DRV_TURBIDITY_ANALOG, &DRV_MOISTURE_ANALOG, &DRV_NTC_ANALOG,
    &DRV_EC_ADS_I2C, &DRV_RTD_SPI, &DRV_PM_UART, &DRV_CO2_I2C, &DRV_SHT_I2C, &DRV_LIGHT_I2C
};

const sensor_driver_t *sensor_driver_find(const char *name)
{
    for (size_t i = 0; i < sizeof ALL_DRIVERS / sizeof ALL_DRIVERS[0]; i++)
        if (strcmp(ALL_DRIVERS[i]->name, name) == 0) return ALL_DRIVERS[i];
    return NULL;
}

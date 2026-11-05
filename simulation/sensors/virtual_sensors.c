/* virtual_sensors.c - environment trace access and virtual bus devices.
 * Each device model is the INVERSE of the physics the firmware driver
 * assumes, written independently so that driver bugs show up in tests. */
#include "virtual_sensors.h"
#include "hal.h"
#include "comms.h"
#include <math.h>
#include <string.h>

/* ---------------- environment ---------------- */
static const env_trace_t *g_env;

void env_set_trace(const env_trace_t *t) { g_env = t; }
uint32_t env_duration_s(void) { return g_env ? g_env->n_rows * g_env->dt_s : 0; }

static uint32_t row_of(uint32_t t)
{
    uint32_t r = t / (g_env->dt_s ? g_env->dt_s : 1);
    return r < g_env->n_rows ? r : g_env->n_rows - 1;
}

int env_sample(int ch, uint32_t t, float *v)
{
    if (!g_env || ch < 0 || ch >= MAX_CHANNELS || !g_env->v[ch]) { *v = NAN; return 1; }
    uint32_t r = row_of(t);
    *v = g_env->v[ch][r];
    return g_env->fault[ch] ? g_env->fault[ch][r] : 0;
}

bool env_link_up(uint32_t t)
{
    if (!g_env || !g_env->link) return true;
    return g_env->link[row_of(t)] != 0;
}

/* ---------------- device models ---------------- */
typedef enum { VD_PH = 1, VD_TURB, VD_MOIST, VD_NTC, VD_ADS_EC, VD_RTD, VD_PM, VD_SCD, VD_SHT, VD_LIGHT } vd_model_t;

typedef struct {
    uint8_t model, bus, rail;
    int8_t  env, env2;          /* env channel(s) feeding this device */
    float   p[3];
    uint8_t state;              /* last command / pointer / mux */
} vdev_t;

#define VDEV_MAX 6
static vdev_t  g_dev[VDEV_MAX];
static int     g_ndev;
static bool    g_rail[8];

static const vdev_t BOARD_WATER[] = {
    {VD_PH,     0,    0, 0, -1, {1.680f, 0.17393f, 0}, 0},   /* electrode Vmid (incl. +10 mV offset), slope */
    {VD_TURB,   1,    1, 1, -1, {3.0f, 0.0025f, 0}, 0},
    {VD_ADS_EC, 0x48, 2, 2,  3, {0.001f, 0.02f, 0}, 0},      /* V per uS/cm, alpha, AIN0 */
    {VD_RTD,    0,    2, 3, -1, {100.0f, 430.0f, 0}, 0},
};
static const vdev_t BOARD_AIR[] = {
    {VD_PM,  HAL_UART_SENSOR, 0, 0, -1, {0}, 0},
    {VD_SCD, 0x62,            1, 1, -1, {0}, 0},
    {VD_SHT, 0x44,            2, 3,  2, {0}, 0},             /* env = temperature, env2 = RH */
};
static const vdev_t BOARD_SOIL[] = {
    {VD_MOIST,  0,    0, 0, -1, {2.8f, 1.3f, 50.0f}, 0},
    {VD_NTC,    1,    1, 1, -1, {10000.0f, 10000.0f, 3950.0f}, 0},
    {VD_ADS_EC, 0x48, 2, 2,  1, {0.0005f, 0.02f, 1}, 0},     /* AIN1, compensated by soil temp */
    {VD_LIGHT,  0x23, 3, 3, -1, {0}, 0},
};

int vdev_load_board(uint8_t profile_id)
{
    const vdev_t *b; int n;
    switch (profile_id) {
    case PROFILE_WATER: b = BOARD_WATER; n = 4; break;
    case PROFILE_AIR:   b = BOARD_AIR;   n = 3; break;
    case PROFILE_SOIL:  b = BOARD_SOIL;  n = 4; break;
    default: g_ndev = 0; memset(g_rail, 0, sizeof g_rail); return -1;
    }
    memcpy(g_dev, b, sizeof(vdev_t) * (size_t)n);
    g_ndev = n;
    memset(g_rail, 0, sizeof g_rail);
    return 0;
}

void vdev_set_rail(uint8_t rail, bool on) { if (rail < 8) g_rail[rail] = on; }

/* A device stops answering if its (only) measured quantity is missing; the
 * SHT-style part measures two quantities and only NACKs if both are missing. */
static bool dev_absent(const vdev_t *v, int f, int f2)
{
    return v->model == VD_SHT ? (f == 1 && f2 == 1) : (f == 1);
}

static vdev_t *find(uint8_t kind_bus, uint8_t addr)
{
    for (int i = 0; i < g_ndev; i++) {
        uint8_t m = g_dev[i].model;
        uint8_t bus = (m == VD_PH || m == VD_TURB || m == VD_MOIST || m == VD_NTC) ? BUS_ADC :
                      (m == VD_RTD) ? BUS_SPI : (m == VD_PM) ? BUS_UART : BUS_I2C;
        if (bus == kind_bus && g_dev[i].bus == addr) return &g_dev[i];
    }
    return NULL;
}

static int sample(const vdev_t *d, int which, float *v)
{
    return env_sample(which ? d->env2 : d->env, hal_timer_now_s(), v);
}

static uint16_t volts_to_adc(float v)
{
    float c = v / HAL_ADC_VREF * (float)HAL_ADC_MAX;
    if (c < 0) c = 0;
    if (c > (float)HAL_ADC_MAX) c = (float)HAL_ADC_MAX;
    return (uint16_t)lrintf(c);
}

int vdev_adc(uint8_t ch, uint16_t *raw)
{
    vdev_t *d = find(BUS_ADC, ch);
    if (!d) { *raw = 0; return HAL_OK; }                 /* floating input reads ~0 */
    if (!g_rail[d->rail]) { *raw = 0; return HAL_OK; }   /* unpowered sensor: 0 V   */
    float x, v = 0;
    int f = sample(d, 0, &x);
    if (f == 1) { *raw = 0; return HAL_OK; }             /* open circuit  */
    if (f == 2) { *raw = HAL_ADC_MAX; return HAL_OK; }   /* shorted / stuck at rail */
    switch (d->model) {
    case VD_PH:    v = d->p[0] - d->p[1] * (x - 7.0f); break;
    case VD_TURB:  v = d->p[0] - d->p[1] * x; break;
    case VD_MOIST: v = d->p[0] - (d->p[0] - d->p[1]) * x / d->p[2]; break;
    case VD_NTC: {
        float r = d->p[0] * expf(d->p[2] * (1.0f / (x + 273.15f) - 1.0f / 298.15f));
        v = HAL_ADC_VREF * r / (r + d->p[1]);
        break; }
    default: break;
    }
    *raw = volts_to_adc(v);
    return HAL_OK;
}

static void put_word(uint8_t *b, uint16_t w, bool corrupt)
{
    b[0] = (uint8_t)(w >> 8); b[1] = (uint8_t)w;
    b[2] = crc8_sensirion(b, 2) ^ (corrupt ? 0x5Au : 0u);
}

static uint16_t clamp_u16(float x) { return x < 0 ? 0 : (x > 65535.0f ? 65535 : (uint16_t)lrintf(x)); }

int vdev_i2c_write(uint8_t addr, const uint8_t *d, size_t len)
{
    vdev_t *v = find(BUS_I2C, addr);
    if (!v || !g_rail[v->rail]) return HAL_ERR_NACK;
    float x, y;
    if (dev_absent(v, sample(v, 0, &x), v->env2 >= 0 ? sample(v, 1, &y) : 0)) return HAL_ERR_NACK;
    if (v->model == VD_ADS_EC && len == 3 && d[0] == 0x01) v->state = (uint8_t)((d[1] >> 4) & 7u); /* MUX */
    return HAL_OK;
}

int vdev_i2c_read(uint8_t addr, uint8_t *d, size_t len)
{
    vdev_t *v = find(BUS_I2C, addr);
    if (!v || !g_rail[v->rail]) return HAL_ERR_NACK;
    float x, y = 0;
    int f = sample(v, 0, &x), f2 = v->env2 >= 0 ? sample(v, 1, &y) : 0;
    if (dev_absent(v, f, f2)) return HAL_ERR_NACK;
    memset(d, 0, len);
    switch (v->model) {
    case VD_ADS_EC: {
        int16_t code;
        if (f == 2) code = 32767;
        else if (v->state != (uint8_t)(4 + (int)v->p[2])) code = 0;        /* other input grounded */
        else {
            float ec_t = x * (1.0f + v->p[1] * (y - 25.0f));             /* EC at actual temperature */
            float c = ec_t * v->p[0] / 4.096f * 32768.0f;
            code = (int16_t)(c > 32767.0f ? 32767 : (c < 0 ? 0 : lrintf(c)));
        }
        if (len >= 2) { d[0] = (uint8_t)((uint16_t)code >> 8); d[1] = (uint8_t)code; }
        break; }
    case VD_SCD:
        if (len >= 9) {
            put_word(&d[0], clamp_u16(x), f == 2);
            put_word(&d[3], 0x6666, false);
            put_word(&d[6], 0x8000, false);
        }
        break;
    case VD_SHT:          /* x = temperature (env), y = RH (env2) */
        if (len >= 6) {
            put_word(&d[0], clamp_u16((x + 45.0f) / 175.0f * 65535.0f), f != 0);
            put_word(&d[3], clamp_u16(y / 100.0f * 65535.0f), f2 != 0);
        }
        break;
    case VD_LIGHT: {
        uint16_t r = (f == 2) ? 0xFFFF : clamp_u16(x * 1.2f);
        if (len >= 2) { d[0] = (uint8_t)(r >> 8); d[1] = (uint8_t)r; }
        break; }
    default: break;
    }
    return HAL_OK;
}

int vdev_spi(uint8_t cs, const uint8_t *tx, uint8_t *rx, size_t len)
{
    vdev_t *v = find(BUS_SPI, cs);
    memset(rx, 0xFF, len);                               /* MISO pull-up if nobody answers */
    if (!v || !g_rail[v->rail] || len < 2) return HAL_OK;
    float t;
    int f = sample(v, 0, &t);
    if (f == 1) return HAL_OK;
    if (tx[0] == 0x00) { rx[1] = 0xC1; return HAL_OK; } /* config register */
    if (tx[0] == 0x01 && len >= 3) {
        const float A = 3.9083e-3f, B = -5.775e-7f;
        float r = v->p[0] * (1.0f + A * t + B * t * t);
        uint16_t code = (uint16_t)(lrintf(r / v->p[1] * 32768.0f) << 1);
        if (f == 2) code |= 1u;                          /* fault bit */
        rx[1] = (uint8_t)(code >> 8); rx[2] = (uint8_t)code;
    }
    return HAL_OK;
}

int vdev_uart_read(uint8_t port, uint8_t *d, size_t len)
{
    vdev_t *v = find(BUS_UART, port);
    if (!v || !g_rail[v->rail] || len < 32) return 0;
    float x;
    int f = sample(v, 0, &x);
    if (f == 1) return 0;                                /* timeout, nothing received */
    uint16_t pm25 = clamp_u16(x), pm1 = clamp_u16(x * 0.7f), pm10 = clamp_u16(x * 1.3f);
    uint16_t w[13] = {pm1, pm25, pm10, pm1, pm25, pm10, 0, 0, 0, 0, 0, 0, 0};
    memset(d, 0, 32);
    d[0] = 0x42; d[1] = 0x4D; d[2] = 0; d[3] = 28;
    for (int i = 0; i < 13; i++) { d[4 + 2 * i] = (uint8_t)(w[i] >> 8); d[5 + 2 * i] = (uint8_t)w[i]; }
    uint16_t s = 0;
    for (int i = 0; i < 30; i++) s = (uint16_t)(s + d[i]);
    if (f == 2) s ^= 0x00FF;                             /* corrupted frame */
    d[30] = (uint8_t)(s >> 8); d[31] = (uint8_t)s;
    return 32;
}

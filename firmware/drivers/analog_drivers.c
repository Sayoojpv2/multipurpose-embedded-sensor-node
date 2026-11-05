/* analog_drivers.c - sensors read through the MCU ADC.
 * Each driver: 4x oversampled ADC read, rail detection, then a documented
 * transfer function with calibration parameters cal.p[]. */
#include "sensor.h"
#include "hal.h"
#include <math.h>

#define ADC_OVERSAMPLE 4
#define ADC_RAIL_LO    2u
#define ADC_RAIL_HI    (HAL_ADC_MAX - 2u)

static float adc_to_volts(int32_t raw) { return (float)raw * HAL_ADC_VREF / (float)HAL_ADC_MAX; }

static int analog_probe(const sensor_channel_cfg_t *c)
{
    uint16_t r;
    return hal_adc_read(c->bus_addr, &r) == HAL_OK ? 0 : -1;
}

static int analog_read(const sensor_channel_cfg_t *c, int32_t *raw)
{
    uint32_t acc = 0;
    for (int i = 0; i < ADC_OVERSAMPLE; i++) {
        uint16_t r;
        if (hal_adc_read(c->bus_addr, &r) != HAL_OK) return RD_NO_RESPONSE;
        acc += r;
    }
    *raw = (int32_t)((acc + ADC_OVERSAMPLE / 2) / ADC_OVERSAMPLE);
    if (*raw <= (int32_t)ADC_RAIL_LO || *raw >= (int32_t)ADC_RAIL_HI) return RD_RAIL;   /* open / short */
    return RD_OK;
}

/* pH electrode + gain stage: V = Vmid - slope*(pH-7)  ->  pH = 7 + (Vmid - V)/slope
 * p0 = Vmid [V] at pH 7, p1 = slope [V/pH] (both from two-point buffer calibration) */
static int ph_convert(const sensor_channel_cfg_t *c, int32_t raw, float aux, float *out)
{
    (void)aux;
    *out = 7.0f + (c->cal.p[0] - adc_to_volts(raw)) / c->cal.p[1];
    return RD_OK;
}

/* Turbidity (optical): V = p0 - p1*NTU  ->  NTU = (p0 - V)/p1 */
static int turb_convert(const sensor_channel_cfg_t *c, int32_t raw, float aux, float *out)
{
    (void)aux;
    float ntu = (c->cal.p[0] - adc_to_volts(raw)) / c->cal.p[1];
    *out = ntu < 0.0f ? 0.0f : ntu;
    return RD_OK;
}

/* Capacitive soil moisture, two-point (dry/wet) calibration:
 * p0 = V_dry (0 %VWC), p1 = V_wet, p2 = %VWC at the wet point */
static int moist_convert(const sensor_channel_cfg_t *c, int32_t raw, float aux, float *out)
{
    (void)aux;
    *out = c->cal.p[2] * (c->cal.p[0] - adc_to_volts(raw)) / (c->cal.p[0] - c->cal.p[1]);
    return RD_OK;
}

/* NTC thermistor to GND with series resistor to Vref (Beta equation):
 * R = Rs*V/(Vref-V);  1/T = 1/T0 + ln(R/R0)/B
 * p0 = R0, p1 = Rs, p2 = T0 [K], p3 = B */
static int ntc_convert(const sensor_channel_cfg_t *c, int32_t raw, float aux, float *out)
{
    (void)aux;
    float v = adc_to_volts(raw);
    if (v <= 0.0f || v >= HAL_ADC_VREF) return RD_OUT_OF_RANGE;
    float r = c->cal.p[1] * v / (HAL_ADC_VREF - v);
    float inv_t = 1.0f / c->cal.p[2] + logf(r / c->cal.p[0]) / c->cal.p[3];
    *out = 1.0f / inv_t - 273.15f;
    return RD_OK;
}

const sensor_driver_t DRV_PH_ANALOG        = {"ph_analog",        BUS_ADC, analog_probe, analog_read, ph_convert};
const sensor_driver_t DRV_TURBIDITY_ANALOG = {"turbidity_analog", BUS_ADC, analog_probe, analog_read, turb_convert};
const sensor_driver_t DRV_MOISTURE_ANALOG  = {"moisture_analog",  BUS_ADC, analog_probe, analog_read, moist_convert};
const sensor_driver_t DRV_NTC_ANALOG       = {"ntc_analog",       BUS_ADC, analog_probe, analog_read, ntc_convert};

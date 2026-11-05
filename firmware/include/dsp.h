/*
 * dsp.h - fixed-buffer signal processing: Hampel outlier detector,
 * moving average and FIR filter. No dynamic memory.
 */
#ifndef DSP_H
#define DSP_H

#include "sensor.h"

#define DSP_FLAG_SUSPECT  0x01  /* sample rejected by outlier detector */
#define DSP_FLAG_RESET    0x02  /* filter state re-initialised         */

typedef struct { float buf[MA_MAX_LEN]; uint8_t len, idx; float sum; bool primed; } ma_filter_t;
typedef struct { const float *h; uint8_t taps, idx; float z[FIR_MAX_TAPS]; bool primed; } fir_filter_t;
typedef struct { float w[HAMPEL_WIN]; uint8_t count, idx; float k, min_dev; } hampel_t;

void  ma_init(ma_filter_t *f, uint8_t len);
float ma_update(ma_filter_t *f, float x);

void  fir_init(fir_filter_t *f, const float *h, uint8_t taps);
float fir_update(fir_filter_t *f, float x);

void  hampel_init(hampel_t *h, float k, float min_dev);
float hampel_update(hampel_t *h, float x, bool *is_outlier);
float dsp_median(const float *v, uint8_t n);

/* Per-channel processing chain: Hampel -> moving average -> optional FIR. */
typedef struct {
    hampel_t hp; ma_filter_t ma; fir_filter_t fir;
    bool use_hampel, use_ma, use_fir;
    uint32_t mac_ops;   /* multiply-accumulate count (processing-load metric) */
} chan_dsp_t;

void  chan_dsp_init(chan_dsp_t *d, const sensor_channel_cfg_t *cfg);
void  chan_dsp_reset(chan_dsp_t *d);
float chan_dsp_process(chan_dsp_t *d, float x, uint8_t *flags);

#endif

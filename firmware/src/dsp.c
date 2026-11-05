/* dsp.c - Hampel outlier detector, moving average and FIR filter. */
#include "dsp.h"
#include <math.h>

/* ---- moving average: running sum over a circular buffer, O(1) ---- */
void ma_init(ma_filter_t *f, uint8_t len)
{
    if (len < 1) len = 1;
    if (len > MA_MAX_LEN) len = MA_MAX_LEN;
    f->len = len; f->idx = 0; f->sum = 0.0f; f->primed = false;
}

float ma_update(ma_filter_t *f, float x)
{
    if (!f->primed) {               /* fill with first sample: no start-up transient */
        for (uint8_t i = 0; i < f->len; i++) f->buf[i] = x;
        f->sum = x * f->len; f->idx = 0; f->primed = true;
        return x;
    }
    f->sum += x - f->buf[f->idx];
    f->buf[f->idx] = x;
    f->idx = (uint8_t)((f->idx + 1) % f->len);
    return f->sum / (float)f->len;
}

/* ---- FIR: y[n] = sum h[k] x[n-k], circular delay line ---- */
void fir_init(fir_filter_t *f, const float *h, uint8_t taps)
{
    if (taps > FIR_MAX_TAPS) taps = FIR_MAX_TAPS;
    f->h = h; f->taps = taps; f->idx = 0; f->primed = false;
}

float fir_update(fir_filter_t *f, float x)
{
    if (f->taps == 0 || f->h == NULL) return x;
    if (!f->primed) {
        for (uint8_t i = 0; i < f->taps; i++) f->z[i] = x;
        f->primed = true;
    }
    f->z[f->idx] = x;                       /* newest sample at idx */
    float y = 0.0f;
    uint8_t j = f->idx;
    for (uint8_t k = 0; k < f->taps; k++) { /* h[0] multiplies newest */
        y += f->h[k] * f->z[j];
        j = (j == 0) ? (uint8_t)(f->taps - 1) : (uint8_t)(j - 1);
    }
    f->idx = (uint8_t)((f->idx + 1) % f->taps);
    return y;
}

/* ---- median of up to HAMPEL_WIN values (insertion sort on a copy) ---- */
float dsp_median(const float *v, uint8_t n)
{
    float a[HAMPEL_WIN];
    if (n == 0) return 0.0f;
    if (n > HAMPEL_WIN) n = HAMPEL_WIN;
    for (uint8_t i = 0; i < n; i++) {
        float x = v[i]; int j = (int)i - 1;
        while (j >= 0 && a[j] > x) { a[j + 1] = a[j]; j--; }
        a[j + 1] = x;
    }
    return (n & 1u) ? a[n / 2] : 0.5f * (a[n / 2 - 1] + a[n / 2]);
}

/* ---- Hampel identifier: |x - median| > max(k * 1.4826 * MAD, min_dev) ---- */
void hampel_init(hampel_t *h, float k, float min_dev)
{
    h->count = 0; h->idx = 0; h->k = k; h->min_dev = min_dev;
}

float hampel_update(hampel_t *h, float x, bool *is_outlier)
{
    *is_outlier = false;
    h->w[h->idx] = x;                         /* window keeps raw samples */
    h->idx = (uint8_t)((h->idx + 1) % HAMPEL_WIN);
    if (h->count < HAMPEL_WIN) h->count++;
    if (h->count < 3) return x;               /* not enough history yet */

    float med = dsp_median(h->w, h->count);
    float dev[HAMPEL_WIN];
    for (uint8_t i = 0; i < h->count; i++) dev[i] = fabsf(h->w[i] - med);
    float mad = dsp_median(dev, h->count);
    float lim = h->k * 1.4826f * mad;
    if (lim < h->min_dev) lim = h->min_dev;
    if (fabsf(x - med) > lim) { *is_outlier = true; return med; }
    return x;
}

/* ---- per-channel chain ---- */
void chan_dsp_init(chan_dsp_t *d, const sensor_channel_cfg_t *c)
{
    d->use_hampel = c->hampel_on != 0;
    d->use_ma = c->ma_len > 1;
    d->use_fir = c->fir_coeffs != NULL && c->fir_taps > 0;
    hampel_init(&d->hp, c->hampel_k, c->hampel_min_dev);
    ma_init(&d->ma, c->ma_len);
    fir_init(&d->fir, c->fir_coeffs, c->fir_taps);
    d->mac_ops = 0;
}

void chan_dsp_reset(chan_dsp_t *d)
{
    d->hp.count = 0; d->hp.idx = 0;
    d->ma.primed = false;
    d->fir.primed = false;
}

float chan_dsp_process(chan_dsp_t *d, float x, uint8_t *flags)
{
    float y = x;
    if (d->use_hampel) {
        bool out;
        y = hampel_update(&d->hp, x, &out);
        if (out) *flags |= DSP_FLAG_SUSPECT;
        d->mac_ops += 2u * HAMPEL_WIN;
    }
    if (d->use_ma)  { y = ma_update(&d->ma, y); d->mac_ops += 2; }
    if (d->use_fir) { y = fir_update(&d->fir, y); d->mac_ops += d->fir.taps; }
    return y;
}

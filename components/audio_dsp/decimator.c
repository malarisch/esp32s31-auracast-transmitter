/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fixed FIR decimator: decim x DECIMATOR_TAPS_PER_DECIM taps of a Kaiser-windowed sinc with cut-off
 * 0.45 / decim of the input rate (the resampler's design at ratio 1.0, without its phase table).
 * Per channel a history of taps - 1 samples (float) bridges the blocks.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "decimator.h"

#define CUTOFF      0.45
#define KAISER_BETA 9.5
#define MAX_DECIM   3
#define MAX_TAPS    (MAX_DECIM * DECIMATOR_TAPS_PER_DECIM)

struct decimator {
    uint8_t channels;
    uint8_t decim;
    uint32_t taps;
    uint32_t max_frames; /* input frames per call the work buffer can take */
    float coef[MAX_TAPS];
    float *hist;        /* channels x (taps - 1), oldest first */
    float *work;        /* (taps - 1) + max_frames: history followed by the block, one channel at a time */
};

static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 50; k++) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-12 * sum) {
            break;
        }
    }
    return sum;
}

static void design(decimator_t *d)
{
    const double cutoff = CUTOFF / d->decim;
    const double half = (d->taps - 1) / 2.0;
    double sum = 0.0;
    double row[MAX_TAPS];
    for (uint32_t k = 0; k < d->taps; k++) {
        double t = (double)k - half;
        double r = t / (half + 1.0);
        double x = 2.0 * cutoff * t;
        double sinc = fabs(x) < 1e-12 ? 1.0 : sin(M_PI * x) / (M_PI * x);
        double win = bessel_i0(KAISER_BETA * sqrt(1.0 - r * r)) / bessel_i0(KAISER_BETA);
        row[k] = 2.0 * cutoff * sinc * win;
        sum += row[k];
    }
    for (uint32_t k = 0; k < d->taps; k++) {
        d->coef[k] = (float)(row[k] / sum);
    }
}

decimator_t *decimator_create(uint8_t channels, uint8_t decim, uint32_t max_frames)
{
    if (channels == 0 || decim < 2 || decim > MAX_DECIM || max_frames == 0) {
        return NULL;
    }
    decimator_t *d = calloc(1, sizeof(*d));
    if (d == NULL) {
        return NULL;
    }
    d->channels = channels;
    d->decim = decim;
    d->taps = (uint32_t)decim * DECIMATOR_TAPS_PER_DECIM;
    d->max_frames = max_frames;
    d->hist = calloc((size_t)channels * (d->taps - 1), sizeof(float));
    d->work = calloc((size_t)(d->taps - 1) + max_frames, sizeof(float));
    if (d->hist == NULL || d->work == NULL) {
        decimator_destroy(d);
        return NULL;
    }
    design(d);
    return d;
}

void decimator_destroy(decimator_t *d)
{
    if (d) {
        free(d->hist);
        free(d->work);
        free(d);
    }
}

void decimator_reset(decimator_t *d)
{
    memset(d->hist, 0, (size_t)d->channels * (d->taps - 1) * sizeof(float));
}

uint32_t decimator_delay_frames(const decimator_t *d)
{
    return d->taps / 2;
}

static int32_t to_i32(float v)
{
    if (v >= 2147483647.0f) {
        return INT32_MAX;
    }
    if (v <= -2147483648.0f) {
        return INT32_MIN;
    }
    return (int32_t)lrintf(v);
}

uint32_t decimator_process(decimator_t *d, const int32_t *in, uint32_t frames, int32_t *const out[])
{
    if (frames > d->max_frames) {
        frames = d->max_frames;
    }
    const uint32_t taps = d->taps, hl = taps - 1, decim = d->decim, nch = d->channels;
    const uint32_t out_frames = frames / decim;
    const uint32_t used = out_frames * decim;
    const float *const coef = d->coef;
    float *const work = d->work;
    for (uint32_t ch = 0; ch < nch; ch++) {
        float *hist = d->hist + (size_t)ch * hl;
        /* work = history ++ this channel's block, so the filter is a plain dot product */
        memcpy(work, hist, hl * sizeof(float));
        for (uint32_t i = 0; i < used; i++) {
            work[hl + i] = (float)in[(size_t)i * nch + ch];
        }
        /* y[m] = sum_k coef[k] * x[m*decim + hl - k]: reversed coefficients over a window ending at the newest sample */
        for (uint32_t m = 0; m < out_frames; m++) {
            const float *x = work + m * decim;   /* x[0] is the oldest sample of the window */
            float acc = 0.0f;
            for (uint32_t k = 0; k < taps; k++) {
                acc += coef[taps - 1 - k] * x[k];
            }
            out[ch][m] = to_i32(acc);
        }
        memcpy(hist, work + used, hl * sizeof(float));
    }
    return out_frames;
}

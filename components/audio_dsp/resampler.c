/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Polyphase windowed-sinc resampler with linear interpolation between phases.
 *
 * Output frame at fractional input position pos (i = floor(pos), mu = pos - i):
 *   y = sum_k x[i - taps/2 + 1 + k] * h(mu + taps/2 - 1 - k),  k = 0..taps-1
 * h() is tabulated for mu = p/PHASES, p = 0..PHASES, and interpolated linearly.
 * Decimating by D scales the filter: D x RESAMPLER_TAPS taps, cut-off 0.45/D of the input rate.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "resampler.h"

#define CUTOFF      0.45    /* normalized to the output rate: 21.6 kHz at 48 kHz, 10.8 at 24, 7.2 at 16 */
#define KAISER_BETA 9.5
#define MAX_DECIM   3

/* Q32.32 position: the top log2(PHASES) fraction bits select the phase, the rest interpolate */
#define PHASE_SHIFT     (32 - 7)
#define PHASE_FRAC_MASK ((1u << PHASE_SHIFT) - 1u)
_Static_assert(RESAMPLER_PHASES == 128, "PHASE_SHIFT assumes 128 phases");

struct resampler {
    uint8_t channels;
    uint8_t decim;
    uint32_t taps;       /* decim * RESAMPLER_TAPS */
    uint32_t half;       /* taps / 2 */
    uint32_t cap;        /* frames per channel buffer */
    uint32_t fill;       /* frames currently buffered */
    uint64_t pos;        /* Q32.32 position of the next output relative to buf[0] */
    uint64_t step;       /* Q32.32 input frames per output frame (ratio * decim) */
    float *buf;          /* channels * cap, planar */
    float *coef;         /* (RESAMPLER_PHASES + 1) rows of taps */
};

static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 50; k++) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < sum * 1e-17) {
            break;
        }
    }
    return sum;
}

static double kernel(double t, uint32_t half, double cutoff)
{
    double r = t / half;
    if (r < -1.0 || r > 1.0) {
        return 0.0;
    }
    double x = 2.0 * cutoff * t;
    double sinc = (fabs(x) < 1e-12) ? 1.0 : sin(M_PI * x) / (M_PI * x);
    double win = bessel_i0(KAISER_BETA * sqrt(1.0 - r * r)) / bessel_i0(KAISER_BETA);
    return 2.0 * cutoff * sinc * win;
}

static void build_table(resampler_t *rs)
{
    const double cutoff = CUTOFF / rs->decim;
    double row[MAX_DECIM * RESAMPLER_TAPS];
    for (int p = 0; p <= RESAMPLER_PHASES; p++) {
        double mu = (double)p / RESAMPLER_PHASES;
        double sum = 0.0;
        for (uint32_t k = 0; k < rs->taps; k++) {
            row[k] = kernel(mu + rs->half - 1 - k, rs->half, cutoff);
            sum += row[k];
        }
        /* Unity DC gain for every phase avoids phase-dependent gain ripple */
        for (uint32_t k = 0; k < rs->taps; k++) {
            rs->coef[(size_t)p * rs->taps + k] = (float)(row[k] / sum);
        }
    }
}

resampler_t *resampler_create_decim(uint8_t channels, uint32_t max_out_frames, uint8_t decim)
{
    if (decim < 1 || decim > MAX_DECIM) {
        return NULL;
    }
    resampler_t *rs = calloc(1, sizeof(*rs));
    if (rs == NULL) {
        return NULL;
    }
    rs->channels = channels;
    rs->decim = decim;
    rs->taps = (uint32_t)decim * RESAMPLER_TAPS;
    rs->half = rs->taps / 2;
    uint32_t max_in = (uint32_t)ceil(max_out_frames * decim * 1.01) + rs->taps + 4;
    rs->cap = rs->taps + max_in;
    rs->buf = calloc((size_t)channels * rs->cap, sizeof(float));
    rs->coef = calloc((size_t)(RESAMPLER_PHASES + 1) * rs->taps, sizeof(float));
    if (rs->buf == NULL || rs->coef == NULL) {
        resampler_destroy(rs);
        return NULL;
    }
    rs->step = (uint64_t)decim << 32;
    build_table(rs);
    resampler_reset(rs);
    return rs;
}

resampler_t *resampler_create(uint8_t channels, uint32_t max_out_frames)
{
    return resampler_create_decim(channels, max_out_frames, 1);
}

void resampler_destroy(resampler_t *rs)
{
    if (rs) {
        free(rs->buf);
        free(rs->coef);
        free(rs);
    }
}

void resampler_reset(resampler_t *rs)
{
    memset(rs->buf, 0, (size_t)rs->channels * rs->cap * sizeof(float));
    /* half-1 frames of zero history so the first output's support starts at buf[0] */
    rs->fill = rs->half - 1;
    rs->pos = (uint64_t)(rs->half - 1) << 32;
}

void resampler_set_ratio(resampler_t *rs, double ratio)
{
    /* Fixed point keeps double (soft-float on the S31) out of the per-sample path */
    rs->step = (uint64_t)llround(ratio * rs->decim * 4294967296.0);
}

/* Buffered frames required to produce outputs up to (and including) position p */
static uint32_t frames_required(const resampler_t *rs, uint64_t p)
{
    return (uint32_t)(p >> 32) + rs->half + 1;
}

uint32_t resampler_input_needed(const resampler_t *rs, uint32_t out_frames)
{
    if (out_frames == 0) {
        return 0;
    }
    uint64_t last = rs->pos + (uint64_t)(out_frames - 1) * rs->step;
    uint32_t req = frames_required(rs, last);
    uint32_t need = req > rs->fill ? req - rs->fill : 0;
    return need + 1; /* one safety frame against rounding at an integer boundary */
}

void resampler_push(resampler_t *rs, const int32_t *in, uint32_t frames)
{
    if (rs->fill + frames > rs->cap) {
        frames = rs->cap - rs->fill;
    }
    const float scale = 1.0f / 2147483648.0f;
    for (uint8_t ch = 0; ch < rs->channels; ch++) {
        float *dst = rs->buf + (size_t)ch * rs->cap + rs->fill;
        const int32_t *src = in + ch;
        for (uint32_t i = 0; i < frames; i++) {
            dst[i] = (float)src[(size_t)i * rs->channels] * scale;
        }
    }
    rs->fill += frames;
}

static int32_t saturate(float y)
{
    float v = y * 2147483648.0f;
    if (v >= 2147483520.0f) {   /* largest float below 2^31 */
        return INT32_MAX;
    }
    if (v <= -2147483648.0f) {
        return INT32_MIN;
    }
    return (int32_t)(v >= 0.0f ? v + 0.5f : v - 0.5f);
}

/* Two dot products sharing x; inlined with a constant n for the common 64-tap case */
static inline __attribute__((always_inline)) void dot2(const float *x, const float *c0, const float *c1,
                                                        uint32_t n, float *y0, float *y1)
{
    float a = 0.0f, b = 0.0f;
    for (uint32_t k = 0; k < n; k++) {
        a += x[k] * c0[k];
        b += x[k] * c1[k];
    }
    *y0 = a;
    *y1 = b;
}

uint32_t resampler_pull(resampler_t *rs, int32_t *const out[], uint32_t out_frames)
{
    const uint64_t pos0 = rs->pos;
    uint32_t produced = 0;

    for (; produced < out_frames; produced++) {
        /* Positions from pos0 by multiplication, consistent with resampler_input_needed() */
        uint64_t p = pos0 + (uint64_t)produced * rs->step;
        if (frames_required(rs, p) > rs->fill) {
            break;
        }
        uint32_t i = (uint32_t)(p >> 32);
        uint32_t frac = (uint32_t)p;
        uint32_t ph = frac >> PHASE_SHIFT;
        float a = (float)(frac & PHASE_FRAC_MASK) * (1.0f / (float)(PHASE_FRAC_MASK + 1u));
        const float *c0 = rs->coef + (size_t)ph * rs->taps;
        const float *c1 = c0 + rs->taps;
        for (uint8_t ch = 0; ch < rs->channels; ch++) {
            const float *x = rs->buf + (size_t)ch * rs->cap + (i - (rs->half - 1));
            float y0, y1;
            if (rs->taps == RESAMPLER_TAPS) {
                dot2(x, c0, c1, RESAMPLER_TAPS, &y0, &y1);
            } else {
                dot2(x, c0, c1, rs->taps, &y0, &y1);
            }
            out[ch][produced] = saturate(y0 + a * (y1 - y0));
        }
    }

    /* Advance and drop frames no longer needed as history */
    uint64_t p_next = pos0 + (uint64_t)produced * rs->step;
    uint32_t drop = (uint32_t)(p_next >> 32) - (rs->half - 1);
    if (drop > rs->fill) {
        drop = rs->fill;
    }
    if (drop > 0) {
        for (uint8_t ch = 0; ch < rs->channels; ch++) {
            float *b = rs->buf + (size_t)ch * rs->cap;
            memmove(b, b + drop, (rs->fill - drop) * sizeof(float));
        }
        rs->fill -= drop;
        p_next -= (uint64_t)drop << 32;
    }
    rs->pos = p_next;
    return produced;
}

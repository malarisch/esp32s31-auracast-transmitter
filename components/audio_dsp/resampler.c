/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Polyphase windowed-sinc resampler with linear interpolation between phases.
 *
 * Output frame at fractional input position pos (i = floor(pos), mu = pos - i):
 *   y = sum_k x[i - TAPS/2 + 1 + k] * h(mu + TAPS/2 - 1 - k),  k = 0..TAPS-1
 * h() is tabulated for mu = p/PHASES, p = 0..PHASES, and interpolated linearly.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "resampler.h"

#define HALF        (RESAMPLER_TAPS / 2)
#define CUTOFF      0.45    /* normalized to the input rate: 21.6 kHz at 48 kHz */
#define KAISER_BETA 9.5

/* Q32.32 position: the top log2(PHASES) fraction bits select the phase, the rest interpolate */
#define PHASE_SHIFT     (32 - 7)
#define PHASE_FRAC_MASK ((1u << PHASE_SHIFT) - 1u)
_Static_assert(RESAMPLER_PHASES == 128, "PHASE_SHIFT assumes 128 phases");

struct resampler {
    uint8_t channels;
    uint32_t cap;        /* frames per channel buffer */
    uint32_t fill;       /* frames currently buffered */
    uint64_t pos;        /* Q32.32 position of the next output relative to buf[0] */
    uint64_t step;       /* Q32.32 ratio (input frames per output frame) */
    float *buf;          /* channels * cap, planar */
    float coef[RESAMPLER_PHASES + 1][RESAMPLER_TAPS];
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

static double kernel(double t)
{
    double r = t / HALF;
    if (r < -1.0 || r > 1.0) {
        return 0.0;
    }
    double x = 2.0 * CUTOFF * t;
    double sinc = (fabs(x) < 1e-12) ? 1.0 : sin(M_PI * x) / (M_PI * x);
    double win = bessel_i0(KAISER_BETA * sqrt(1.0 - r * r)) / bessel_i0(KAISER_BETA);
    return 2.0 * CUTOFF * sinc * win;
}

static void build_table(resampler_t *rs)
{
    for (int p = 0; p <= RESAMPLER_PHASES; p++) {
        double mu = (double)p / RESAMPLER_PHASES;
        double row[RESAMPLER_TAPS];
        double sum = 0.0;
        for (int k = 0; k < RESAMPLER_TAPS; k++) {
            row[k] = kernel(mu + HALF - 1 - k);
            sum += row[k];
        }
        /* Unity DC gain for every phase avoids phase-dependent gain ripple */
        for (int k = 0; k < RESAMPLER_TAPS; k++) {
            rs->coef[p][k] = (float)(row[k] / sum);
        }
    }
}

resampler_t *resampler_create(uint8_t channels, uint32_t max_out_frames)
{
    resampler_t *rs = calloc(1, sizeof(*rs));
    if (rs == NULL) {
        return NULL;
    }
    uint32_t max_in = (uint32_t)ceil(max_out_frames * 1.01) + RESAMPLER_TAPS + 4;
    rs->channels = channels;
    rs->cap = RESAMPLER_TAPS + max_in;
    rs->buf = calloc((size_t)channels * rs->cap, sizeof(float));
    if (rs->buf == NULL) {
        free(rs);
        return NULL;
    }
    rs->step = (uint64_t)1 << 32;
    build_table(rs);
    resampler_reset(rs);
    return rs;
}

void resampler_destroy(resampler_t *rs)
{
    if (rs) {
        free(rs->buf);
        free(rs);
    }
}

void resampler_reset(resampler_t *rs)
{
    memset(rs->buf, 0, (size_t)rs->channels * rs->cap * sizeof(float));
    /* HALF-1 frames of zero history so the first output's support starts at buf[0] */
    rs->fill = HALF - 1;
    rs->pos = (uint64_t)(HALF - 1) << 32;
}

void resampler_set_ratio(resampler_t *rs, double ratio)
{
    /* Fixed point keeps double (soft-float on the S31) out of the per-sample path */
    rs->step = (uint64_t)llround(ratio * 4294967296.0);
}

/* Buffered frames required to produce outputs up to (and including) position p */
static uint32_t frames_required(uint64_t p)
{
    return (uint32_t)(p >> 32) + HALF + 1;
}

uint32_t resampler_input_needed(const resampler_t *rs, uint32_t out_frames)
{
    if (out_frames == 0) {
        return 0;
    }
    uint64_t last = rs->pos + (uint64_t)(out_frames - 1) * rs->step;
    uint32_t req = frames_required(last);
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

uint32_t resampler_pull(resampler_t *rs, int32_t *const out[], uint32_t out_frames)
{
    const uint64_t pos0 = rs->pos;
    uint32_t produced = 0;

    for (; produced < out_frames; produced++) {
        /* Positions from pos0 by multiplication, consistent with resampler_input_needed() */
        uint64_t p = pos0 + (uint64_t)produced * rs->step;
        if (frames_required(p) > rs->fill) {
            break;
        }
        uint32_t i = (uint32_t)(p >> 32);
        uint32_t frac = (uint32_t)p;
        uint32_t ph = frac >> PHASE_SHIFT;
        float a = (float)(frac & PHASE_FRAC_MASK) * (1.0f / (float)(PHASE_FRAC_MASK + 1u));
        const float *c0 = rs->coef[ph];
        const float *c1 = rs->coef[ph + 1];
        for (uint8_t ch = 0; ch < rs->channels; ch++) {
            const float *x = rs->buf + (size_t)ch * rs->cap + (i - (HALF - 1));
            float y0 = 0.0f, y1 = 0.0f;
            for (int k = 0; k < RESAMPLER_TAPS; k++) {
                y0 += x[k] * c0[k];
                y1 += x[k] * c1[k];
            }
            out[ch][produced] = saturate(y0 + a * (y1 - y0));
        }
    }

    /* Advance and drop frames no longer needed as history */
    uint64_t p_next = pos0 + (uint64_t)produced * rs->step;
    uint32_t drop = (uint32_t)(p_next >> 32) - (HALF - 1);
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

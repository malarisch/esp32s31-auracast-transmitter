/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RESAMPLER_TAPS   64
#define RESAMPLER_PHASES 128

/**
 * Variable-ratio polyphase resampler (windowed sinc, float internally) for small
 * ratio deviations around 1.0, used to cross from the I2S clock domain into the
 * Bluetooth clock domain. Input is interleaved int32, output is planar int32.
 */
typedef struct resampler resampler_t;

/* Allocates once; no allocation afterwards. Returns NULL on failure. */
resampler_t *resampler_create(uint8_t channels, uint32_t max_out_frames);

/*
 * Same with decimation by decim (1, 2 or 3): output rate = input rate / decim, anti-alias cut-off at
 * 0.45 x the output rate, decim * RESAMPLER_TAPS taps. The ratio keeps its meaning (about 1.0),
 * resampler_input_needed() counts input frames.
 */
resampler_t *resampler_create_decim(uint8_t channels, uint32_t max_out_frames, uint8_t decim);
void resampler_destroy(resampler_t *rs);

/* Zero the history and restart at position 0. */
void resampler_reset(resampler_t *rs);

/* Input frames consumed per output frame, divided by decim; valid range (0.99, 1.01). */
void resampler_set_ratio(resampler_t *rs, double ratio);

/* Frames to push so that resampler_pull(out_frames) returns out_frames. */
uint32_t resampler_input_needed(const resampler_t *rs, uint32_t out_frames);

/* Interleaved int32 input, full scale = INT32_MAX. */
void resampler_push(resampler_t *rs, const int32_t *in, uint32_t frames);

/* Planar int32 output (out[ch][frame]), saturating. Returns frames produced. */
uint32_t resampler_pull(resampler_t *rs, int32_t *const out[], uint32_t out_frames);

#ifdef __cplusplus
}
#endif

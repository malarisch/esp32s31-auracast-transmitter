/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Taps per unit of decimation: 128 taps for 2:1, 192 for 3:1 (same design as the resampler) */
#define DECIMATOR_TAPS_PER_DECIM 64

/**
 * Fixed-ratio FIR decimator (windowed sinc, Kaiser window, cut-off 0.45 x the output rate), used for
 * the Standard Quality BIS when the drift-corrected signal is already at 48 kHz. Interleaved int32 input,
 * planar int32 output. A few hundred bytes of coefficients, unlike the polyphase resampler's tables.
 */
typedef struct decimator decimator_t;

/* decim 2 or 3; max_frames = input frames per decimator_process() call. Allocates once; NULL on failure. */
decimator_t *decimator_create(uint8_t channels, uint8_t decim, uint32_t max_frames);
void decimator_destroy(decimator_t *d);

/* Zero the history. */
void decimator_reset(decimator_t *d);

/* Group delay in input frames, rounded up: taps / 2 (63.5 -> 64 for 2:1, 95.5 -> 96 for 3:1) */
uint32_t decimator_delay_frames(const decimator_t *d);

/*
 * Consumes `frames` interleaved input frames (a multiple of decim, at most max_frames) and writes
 * frames / decim planar output frames to out[ch][]. Returns the output frames produced.
 */
uint32_t decimator_process(decimator_t *d, const int32_t *in, uint32_t frames, int32_t *const out[]);

#ifdef __cplusplus
}
#endif

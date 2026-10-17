/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "spsc_ring.h"

#define I2S_IN_DMA_FRAMES 240

typedef struct {
    int bclk_gpio;
    int ws_gpio;
    int din_gpio;
    uint32_t sample_rate_hz;
} i2s_in_cfg_t;

typedef struct {
    uint32_t ring_count;         /* frames in the ring as of the last DMA interrupt (minus consumed) */
    int64_t t_last_isr_us;       /* esp_timer time of the last on_recv */
    uint64_t frames_total;       /* frames delivered by DMA since start */
    uint32_t overflow_frames;    /* frames dropped because the ring was full */
    bool signal_present;         /* last on_recv less than 20 ms ago */
} i2s_in_snapshot_t;

/*
 * Starts I2S0 as Philips slave receiver (2 x 32-bit slots). The DMA interrupt copies
 * every block into `ring`. Call from the task/core that should own the interrupt.
 */
esp_err_t i2s_in_start(const i2s_in_cfg_t *cfg, spsc_ring_t *ring);

/* Consistent snapshot of the input state (seqlock against the DMA interrupt) */
void i2s_in_snapshot(i2s_in_snapshot_t *out);

/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "ctrl_regs.h"

typedef struct {
    uint8_t state;          /* AURA_PIPE_* */
    bool signal;            /* I2S clock present */
    uint32_t fifo_fill;     /* frames @ 48 kHz (control loop estimate in RUN, ring count otherwise) */
    float drift_ppm;        /* drift controller correction */
    float in_ppm;           /* measured input rate vs. 48 kHz */
    uint32_t underrun;
    uint32_t overflow;
    uint32_t lost;
    uint32_t i2s_bits;      /* OR of the I2S slot words since the previous call */
} pipeline_status_t;

/*
 * Creates the encoder task (priority 15, pinned to core 1) for cfg (channel mode, FIFO set point).
 * Call after broadcast_configure() and before broadcast_start().
 */
esp_err_t pipeline_start(const ctrl_config_t *cfg);

/* Current state for the I2C status registers (any task) */
void pipeline_get_status(pipeline_status_t *out);

/* broadcast_tick_cb_t: wakes the encoder task (called from the ISO task) */
void pipeline_tick(void *arg);

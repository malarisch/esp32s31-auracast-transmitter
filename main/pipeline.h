/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

/* Creates the encoder task (priority 15, pinned to core 1). Call before broadcast_start(). */
esp_err_t pipeline_start(void);

/* broadcast_tick_cb_t: wakes the encoder task (called from the ISO task) */
void pipeline_tick(void *arg);

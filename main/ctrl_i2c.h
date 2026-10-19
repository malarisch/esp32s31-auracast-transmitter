/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * I2C slave front end of the control interface (protocol: auracast_i2c_regs.h).
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#include "ctrl_regs.h"

/* Starts the I2C slave and its task (core 0). With CONFIG_AURACAST_AUTOSTART a START is issued at once. */
esp_err_t ctrl_i2c_start(void);

/* Blocks until the master sent START with a valid configuration (state is STARTING then) */
void ctrl_i2c_wait_start(ctrl_config_t *out);

/* STREAMING is set by the status refresh once all BIS stream; use this for FAULT */
void ctrl_i2c_set_state(uint8_t state, uint8_t err);

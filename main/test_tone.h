/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Test mode without FPGA: I2S1 runs as master (XTAL clock, deliberately off-nominal
 * rate) and drives 440 Hz left / 880 Hz right at -12 dBFS onto the given pins. The
 * I2S0 slave inputs are then routed back from the same pads through the GPIO matrix.
 * Must be called after i2s_in_start(). Never enable with the FPGA connected.
 */
esp_err_t test_tone_start(int bclk_gpio, int ws_gpio, int dout_gpio, uint32_t rate_hz);

/* false stops the I2S1 channel, so BCLK/WS stop toggling (emulates clock loss) */
void test_tone_set_enabled(bool on);

/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <stdatomic.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_rom_gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/i2s_periph.h"

#include "test_tone.h"

/* 1200 frames hold exactly 11 periods of 440 Hz and 22 periods of 880 Hz at 48 kHz */
#define TABLE_FRAMES 1200
#define AMPLITUDE    0.25   /* -12 dBFS */

static const char *TAG = "test_tone";

static i2s_chan_handle_t s_tx;
static int32_t s_table[TABLE_FRAMES * 2];
static atomic_bool s_enabled;

static void writer_task(void *arg)
{
    for (;;) {
        if (!atomic_load(&s_enabled)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx, s_table, sizeof(s_table), &written, 100);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

/* Route the I2S0 slave inputs from pads that I2S1 drives as outputs */
static void loop_back_to_i2s0(int gpio, uint32_t in_sig)
{
    gpio_input_enable(gpio);
    esp_rom_gpio_connect_in_signal(gpio, in_sig, false);
}

esp_err_t test_tone_start(int bclk_gpio, int ws_gpio, int dout_gpio, uint32_t rate_hz)
{
    for (int i = 0; i < TABLE_FRAMES; i++) {
        double t = (double)i / 48000.0;
        s_table[2 * i + 0] = (int32_t)lrint(AMPLITUDE * 2147483647.0 * sin(2.0 * M_PI * 440.0 * t));
        s_table[2 * i + 1] = (int32_t)lrint(AMPLITUDE * 2147483647.0 * sin(2.0 * M_PI * 880.0 * t));
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "new channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = bclk_gpio,
            .ws = ws_gpio,
            .dout = dout_gpio,
            .din = I2S_GPIO_UNUSED,
        },
    };
    /* The APLL belongs to the I2S0 slave; XTAL with the fractional divider still yields the off-nominal rate */
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_XTAL;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "init std");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "enable");
    atomic_store(&s_enabled, true);

    loop_back_to_i2s0(bclk_gpio, i2s_periph_signal[I2S_NUM_0].s_rx_bck_sig);
    loop_back_to_i2s0(ws_gpio, i2s_periph_signal[I2S_NUM_0].s_rx_ws_sig);
    loop_back_to_i2s0(dout_gpio, i2s_periph_signal[I2S_NUM_0].data_in_sigs[0]);

    if (xTaskCreatePinnedToCore(writer_task, "test_tone", 3072, NULL, 10, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGW(TAG, "I2S1 master test tone at %lu Hz (XTAL) looped back into the I2S input — FPGA must be disconnected",
             (unsigned long)rate_hz);
    return ESP_OK;
}

void test_tone_set_enabled(bool on)
{
    if (on) {
        if (!atomic_load(&s_enabled)) {
            i2s_channel_enable(s_tx);
            atomic_store(&s_enabled, true);
        }
    } else if (atomic_load(&s_enabled)) {
        atomic_store(&s_enabled, false);
        i2s_channel_disable(s_tx);
    }
}

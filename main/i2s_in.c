/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * I2S0 slave receiver. The FPGA drives BCLK/WS; every DMA block is copied into the
 * SPSC ring from the interrupt, together with a timestamp used to interpolate the
 * FIFO fill level between interrupts.
 */
#include <stdatomic.h>

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_timer.h"

#include "i2s_in.h"

#define SIGNAL_TIMEOUT_US 20000

static const char *TAG = "i2s_in";

static i2s_chan_handle_t s_rx;
static spsc_ring_t *s_ring;

/* Seqlock: odd while the interrupt updates the fields below */
static _Atomic uint32_t s_seq;
static volatile int64_t s_t_last_isr_us;
static volatile uint64_t s_frames_total;
static volatile uint32_t s_overflow_frames;
static volatile uint32_t s_ring_w;   /* ring write index right after this block */

static bool on_recv(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user_ctx)
{
    uint32_t frames = event->size / (2 * sizeof(int32_t));
    uint32_t written = spsc_ring_write(s_ring, event->dma_buf, frames);
    int64_t now = esp_timer_get_time();

    atomic_fetch_add_explicit(&s_seq, 1, memory_order_acq_rel);
    s_t_last_isr_us = now;
    s_frames_total += frames;
    s_overflow_frames += frames - written;
    s_ring_w = atomic_load_explicit(&s_ring->w, memory_order_relaxed);
    atomic_fetch_add_explicit(&s_seq, 1, memory_order_release);
    return false;
}

esp_err_t i2s_in_start(const i2s_in_cfg_t *cfg, spsc_ring_t *ring)
{
    s_ring = ring;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_SLAVE);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = I2S_IN_DMA_FRAMES;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx), TAG, "new channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(cfg->sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = cfg->bclk_gpio,
            .ws = cfg->ws_gpio,
            .dout = I2S_GPIO_UNUSED,
            .din = cfg->din_gpio,
        },
    };
    /*
     * The slave samples BCLK with an internal clock of >= 8 x BCLK (24.576 MHz for 2 x 32 bit
     * at 48 kHz) and needs a source divider >= 2. XTAL (40 MHz) cannot do that on the S31, so
     * use the APLL. Its exact frequency does not matter in slave mode.
     */
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg), TAG, "init std");

    i2s_event_callbacks_t cbs = {
        .on_recv = on_recv,
    };
    ESP_RETURN_ON_ERROR(i2s_channel_register_event_callback(s_rx, &cbs, NULL), TAG, "callbacks");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "enable");
    ESP_LOGI(TAG, "slave RX on BCLK=%d WS=%d DIN=%d, %lu Hz", cfg->bclk_gpio, cfg->ws_gpio,
             cfg->din_gpio, (unsigned long)cfg->sample_rate_hz);
    return ESP_OK;
}

void i2s_in_snapshot(i2s_in_snapshot_t *out)
{
    uint32_t s0, s1, ring_w;
    do {
        s0 = atomic_load_explicit(&s_seq, memory_order_acquire);
        out->t_last_isr_us = s_t_last_isr_us;
        out->frames_total = s_frames_total;
        out->overflow_frames = s_overflow_frames;
        ring_w = s_ring_w;
        atomic_thread_fence(memory_order_acquire);
        s1 = atomic_load_explicit(&s_seq, memory_order_relaxed);
    } while ((s0 & 1) || s0 != s1);

    /* Count against the write index of the same block as the timestamp; r is only moved by the caller */
    out->ring_count = ring_w - atomic_load_explicit(&s_ring->r, memory_order_acquire);
    out->signal_present = out->frames_total > 0 &&
                          (esp_timer_get_time() - out->t_last_isr_us) < SIGNAL_TIMEOUT_US;
}

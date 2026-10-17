/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "i2s_in.h"
#include "spsc_ring.h"
#include "test_tone.h"

static const char *TAG = "main";

#if CONFIG_AURACAST_SOURCE_I2S
#define DIAG_RING_FRAMES 4096
#define DIAG_CHUNK       512

static spsc_ring_t s_ring;
static int32_t s_ring_buf[DIAG_RING_FRAMES * 2];
static int32_t s_chunk[DIAG_CHUNK * 2];

/*
 * Temporary input diagnostics (replaced by the pipeline): drains the ring, measures the
 * incoming sample rate against the local clock and the tone frequency per channel.
 */
static void i2s_diag_task(void *arg)
{
    spsc_ring_init(&s_ring, s_ring_buf, DIAG_RING_FRAMES, 2);
    const i2s_in_cfg_t cfg = {
        .bclk_gpio = CONFIG_AURACAST_I2S_BCLK_GPIO,
        .ws_gpio = CONFIG_AURACAST_I2S_WS_GPIO,
        .din_gpio = CONFIG_AURACAST_I2S_DIN_GPIO,
        .sample_rate_hz = 48000,
    };
    ESP_ERROR_CHECK(i2s_in_start(&cfg, &s_ring));
#if CONFIG_AURACAST_TEST_TONE
    vTaskDelay(pdMS_TO_TICKS(CONFIG_AURACAST_TEST_TONE_START_DELAY_MS));
    ESP_ERROR_CHECK(test_tone_start(cfg.bclk_gpio, cfg.ws_gpio, cfg.din_gpio, CONFIG_AURACAST_TEST_TONE_RATE_HZ));
#endif

    i2s_in_snapshot_t first = {0}, snap;
    bool have_first = false;
    uint32_t cross_l = 0, cross_r = 0;
    uint64_t frames_read = 0;
    int32_t prev_l = 0, prev_r = 0;
    int64_t next_log = esp_timer_get_time() + 5000000;

    for (;;) {
        uint32_t n;
        while ((n = spsc_ring_read(&s_ring, s_chunk, DIAG_CHUNK)) > 0) {
            for (uint32_t i = 0; i < n; i++) {
                int32_t l = s_chunk[2 * i], r = s_chunk[2 * i + 1];
                cross_l += (prev_l < 0 && l >= 0);
                cross_r += (prev_r < 0 && r >= 0);
                prev_l = l;
                prev_r = r;
            }
            frames_read += n;
        }
        vTaskDelay(pdMS_TO_TICKS(20));

        if (esp_timer_get_time() < next_log) {
            continue;
        }
        next_log += 5000000;
        i2s_in_snapshot(&snap);
        double rate_ppm = 0.0;
        if (!have_first && snap.signal_present) {
            first = snap;
            have_first = true;
        } else if (have_first && snap.t_last_isr_us > first.t_last_isr_us) {
            double dt = (double)(snap.t_last_isr_us - first.t_last_isr_us) * 1e-6;
            rate_ppm = ((double)(snap.frames_total - first.frames_total) / dt / 48000.0 - 1.0) * 1e6;
        }
        double secs = frames_read / 48000.0;
        ESP_LOGI("i2s_in", "rate_ppm=%+.2f tone_l=%.1fHz tone_r=%.1fHz ovf=%lu present=%d",
                 rate_ppm, secs > 0 ? cross_l / secs : 0.0, secs > 0 ? cross_r / secs : 0.0,
                 (unsigned long)snap.overflow_frames, snap.signal_present);
        cross_l = cross_r = 0;
        frames_read = 0;
    }
}
#endif

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "auracast-s31 boot, IDF %s", esp_get_idf_version());

#if CONFIG_AURACAST_SOURCE_I2S
    xTaskCreatePinnedToCore(i2s_diag_task, "i2s_diag", 4096, NULL, 12, NULL, 1);
#endif
}

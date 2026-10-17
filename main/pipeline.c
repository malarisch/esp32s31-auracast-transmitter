/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Encoder task: produces one SDU pair (left/right LC3 frame) whenever the controller
 * has room in its queue, paced by the BIS "sent" callbacks.
 */
#include <math.h>
#include <string.h>

#include "esp_lc3_enc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "broadcast.h"
#include "pipeline.h"

#define FRAMES          480     /* 10 ms at 48 kHz */
#define MAX_SDU         160
#define WATCHDOG_MS     50
#define STATS_PERIOD_US 5000000
#define SYNTH_AMPLITUDE 0.25    /* -12 dBFS */

static const char *TAG = "pipeline";

static TaskHandle_t s_task;
static int32_t s_pcm[2][FRAMES];
static uint8_t s_sdu[2][MAX_SDU];
static void *s_enc[2];
static uint16_t s_sdu_len;

static const char *s_state = "INIT";
static uint32_t s_pairs;
static uint32_t s_proc_us_max;

void pipeline_tick(void *arg)
{
    TaskHandle_t task = s_task;
    if (task) {
        xTaskNotifyGive(task);
    }
}

static esp_err_t encoders_open(void)
{
    for (int ch = 0; ch < 2; ch++) {
        esp_lc3_enc_config_t cfg = ESP_LC3_ENC_CONFIG_DEFAULT();
        cfg.sample_rate = 48000;
        cfg.bits_per_sample = 32;
        cfg.channel = 1;
        cfg.frame_dms = 100;
        cfg.nbyte = s_sdu_len;
        if (esp_lc3_enc_open(&cfg, sizeof(cfg), &s_enc[ch]) != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "LC3 encoder %d open failed", ch);
            return ESP_FAIL;
        }
    }
    int in_size = 0, out_size = 0;
    esp_lc3_enc_get_frame_size(s_enc[0], &in_size, &out_size);
    ESP_LOGI(TAG, "LC3 encoders: in %d bytes, out %d bytes per frame", in_size, out_size);
    return in_size == FRAMES * (int)sizeof(int32_t) ? ESP_OK : ESP_FAIL;
}

static bool encode(int ch)
{
    esp_audio_enc_in_frame_t in = {
        .buffer = (uint8_t *)s_pcm[ch],
        .len = FRAMES * sizeof(int32_t),
    };
    esp_audio_enc_out_frame_t out = {
        .buffer = s_sdu[ch],
        .len = s_sdu_len,
    };
    return esp_lc3_enc_process(s_enc[ch], &in, &out) == ESP_AUDIO_ERR_OK && out.encoded_bytes == s_sdu_len;
}

#if CONFIG_AURACAST_SOURCE_SYNTH
/* 1200 frames hold exactly 11 periods of 440 Hz and 22 of 880 Hz; computed once (double is soft-float here) */
#define SYNTH_TABLE_FRAMES 1200
static int32_t s_synth[2][SYNTH_TABLE_FRAMES];

static void synth_init(void)
{
    static const double freq[2] = {440.0, 880.0};
    for (int ch = 0; ch < 2; ch++) {
        for (int i = 0; i < SYNTH_TABLE_FRAMES; i++) {
            s_synth[ch][i] = (int32_t)lrint(SYNTH_AMPLITUDE * 2147483647.0 * sin(2.0 * M_PI * freq[ch] * i / 48000.0));
        }
    }
}

static void synth_fill(void)
{
    static uint32_t pos;
    for (int i = 0; i < FRAMES; i++) {
        s_pcm[0][i] = s_synth[0][pos];
        s_pcm[1][i] = s_synth[1][pos];
        pos = (pos + 1) % SYNTH_TABLE_FRAMES;
    }
}
#endif

static void produce_pair(void)
{
    int64_t t0 = esp_timer_get_time();
#if CONFIG_AURACAST_SOURCE_SYNTH
    s_state = "SYNTH";
    synth_fill();
#else
    /* I2S path follows in the next step; until then send silence */
    s_state = "NO_SIGNAL";
    memset(s_pcm, 0, sizeof(s_pcm));
#endif
    bool ok = encode(0) && encode(1);
    s_pairs++;
    if (!ok) {
        ESP_LOGE(TAG, "LC3 encode failed");
    }
#if CONFIG_AURACAST_DEBUG_SKIP_SEND_EVERY > 0
    if (s_pairs % CONFIG_AURACAST_DEBUG_SKIP_SEND_EVERY == 0) {
        return;
    }
#endif
    broadcast_send_pair(s_sdu[0], s_sdu[1], s_sdu_len);
    uint32_t proc = (uint32_t)(esp_timer_get_time() - t0);
    s_proc_us_max = proc > s_proc_us_max ? proc : s_proc_us_max;
}

static void log_stats(void)
{
    broadcast_stats_t st;
    broadcast_get_stats(&st, true);
    ESP_LOGI(TAG, "state=%s fill=%.1f ppm=%+.2f underrun=%u ovf=%u lost=%u senderr=%lu wd=%lu "
             "tick_us=[%lu..%lu] mean=%.1f proc_us_max=%lu sent=%lu heap=%lu",
             s_state, 0.0, 0.0, 0u, 0u, 0u, (unsigned long)st.send_err, (unsigned long)st.wd_refills,
             (unsigned long)st.tick_us_min, (unsigned long)st.tick_us_max, st.tick_us_mean,
             (unsigned long)s_proc_us_max, (unsigned long)st.sent,
             (unsigned long)esp_get_free_heap_size());
    s_proc_us_max = 0;
}

static void encoder_task(void *arg)
{
    s_sdu_len = broadcast_sdu_len();
    if (s_sdu_len > MAX_SDU || encoders_open() != ESP_OK) {
        ESP_LOGE(TAG, "encoder setup failed (sdu %u)", s_sdu_len);
        vTaskDelete(NULL);
        return;
    }
#if CONFIG_AURACAST_SOURCE_SYNTH
    synth_init();
#endif
    s_task = xTaskGetCurrentTaskHandle();
    int64_t next_log = esp_timer_get_time() + STATS_PERIOD_US;

    for (;;) {
        uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WATCHDOG_MS));
        if (notified == 0 && broadcast_streaming()) {
            /* No confirmation for 50 ms: assume the controller queue ran dry and refill it */
            broadcast_reset_in_flight();
        }
        for (uint32_t n = broadcast_pairs_wanted(); n > 0; n--) {
            produce_pair();
        }
        if (esp_timer_get_time() >= next_log) {
            next_log += STATS_PERIOD_US;
            log_stats();
        }
    }
}

esp_err_t pipeline_start(void)
{
    if (xTaskCreatePinnedToCore(encoder_task, "encoder", 6144, NULL, 15, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

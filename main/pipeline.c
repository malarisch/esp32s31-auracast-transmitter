/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Encoder task: produces one SDU set (one LC3 frame per BIS: left/right for stereo, one channel or
 * the (L+R)/2 downmix for mono) whenever the controller has room in its queue, paced by the BIS "sent" callbacks
 * (Bluetooth clock domain).
 *
 * I2S source: 48 kHz frames arrive in the FPGA's (PTP) clock domain through the SPSC ring.
 * A PI controller steers the resampler ratio from the ring fill level, so exactly one LC3
 * frame per channel (480/240/160 frames at 48/24/16 kHz) leaves per 10 ms of Bluetooth time.
 *
 *   NO_SIGNAL --signal--> PRIME --fill >= set point--> RUN
 *   RUN --signal lost--> NO_SIGNAL, RUN --underrun/overflow--> PRIME,
 *   RUN --ratio pinned at the clamp--> RATE_ERROR --5 s--> NO_SIGNAL
 * Outside RUN, encoded silence keeps the broadcast alive.
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

#include "auracast_i2c_regs.h"
#include "broadcast.h"
#include "pipeline.h"

#if CONFIG_AURACAST_SOURCE_I2S
#include "drift_ctrl.h"
#include "i2s_in.h"
#include "rate_check.h"
#include "resampler.h"
#include "spsc_ring.h"
#include "test_tone.h"
#endif

#define FRAMES          480     /* 10 ms at 48 kHz: I2S input per tick, and the largest LC3 frame */
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
static uint8_t s_nch;           /* BIS = encoder channels */
static uint32_t s_out_frames;   /* LC3 frame length at the BIS sample rate */
static uint8_t s_channels;      /* AURA_CH_*: how the stereo input maps to the BIS */
static uint16_t s_fifo_target;  /* input FIFO set point, frames @ 48 kHz */

/* I2S slot bit mask: one reader in i2s_in, two consumers here (log and status) */
static portMUX_TYPE s_bits_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_bits_acc[2];
enum { BITS_LOG, BITS_STATUS };

static volatile bool s_source_ready;   /* source_init() done: the I2S input may be queried */

static uint32_t s_frames_made;

/*
 * Window statistics: accumulated by the encoder task, read and reset by the stats task.
 * The encoder task never logs periodically: a blocking UART write would starve the
 * controller queue (2 SDUs = 20 ms of slack).
 */
typedef struct {
    double ppm_sum;
    double fill_sum;
    uint32_t ctrl_n;
    uint32_t cross[2];
    uint32_t cross_frames;
    uint32_t proc_us_max;
} window_stats_t;

static portMUX_TYPE s_win_lock = portMUX_INITIALIZER_UNLOCKED;
static window_stats_t s_win;

void pipeline_tick(void *arg)
{
    TaskHandle_t task = s_task;
    if (task) {
        xTaskNotifyGive(task);
    }
}

static esp_err_t encoders_open(void)
{
    for (int ch = 0; ch < s_nch; ch++) {
        esp_lc3_enc_config_t cfg = ESP_LC3_ENC_CONFIG_DEFAULT();
        cfg.sample_rate = broadcast_bis_rate(0);
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
    return in_size == (int)(s_out_frames * sizeof(int32_t)) ? ESP_OK : ESP_FAIL;
}

static bool encode(int ch)
{
    esp_audio_enc_in_frame_t in = {
        .buffer = (uint8_t *)s_pcm[ch],
        .len = s_out_frames * sizeof(int32_t),
    };
    esp_audio_enc_out_frame_t out = {
        .buffer = s_sdu[ch],
        .len = s_sdu_len,
    };
    return esp_lc3_enc_process(s_enc[ch], &in, &out) == ESP_AUDIO_ERR_OK && out.encoded_bytes == s_sdu_len;
}

static void silence(void)
{
    memset(s_pcm, 0, sizeof(s_pcm));
}

#if CONFIG_AURACAST_SOURCE_SYNTH
/*
 * 25 ms hold exactly 11 periods of 440 Hz and 22 of 880 Hz (1200/600/400 frames at 48/24/16 kHz);
 * computed once (double is soft-float here)
 */
#define SYNTH_TABLE_FRAMES 1200
static int32_t s_synth[2][SYNTH_TABLE_FRAMES];
static uint32_t s_synth_len;

static void source_init(void)
{
    static const double freq[2] = {440.0, 880.0};
    const uint32_t rate = broadcast_bis_rate(0);
    s_synth_len = rate / 40;
    for (int ch = 0; ch < s_nch; ch++) {
        for (uint32_t i = 0; i < s_synth_len; i++) {
            s_synth[ch][i] = (int32_t)lrint(SYNTH_AMPLITUDE * 2147483647.0 * sin(2.0 * M_PI * freq[ch] * i / rate));
        }
    }
}

static void source_fill(void)
{
    static uint32_t pos;
    for (uint32_t i = 0; i < s_out_frames; i++) {
        for (int ch = 0; ch < s_nch; ch++) {
            s_pcm[ch][i] = s_synth[ch][pos];
        }
        pos = (pos + 1) % s_synth_len;
    }
}

static const char *source_state(void)
{
    return "SYNTH";
}

static void source_log_fields(const window_stats_t *w, double *fill, double *ppm, double *in_ppm,
                              uint32_t *underrun, uint32_t *ovf, uint32_t *lost)
{
    *fill = *ppm = *in_ppm = 0.0;
    *underrun = *ovf = *lost = 0;
}
#else /* CONFIG_AURACAST_SOURCE_I2S */

#define RING_FRAMES     4096
#define IN_MAX_FRAMES   (FRAMES + FRAMES / 50 + 3 * RESAMPLER_TAPS + 8)   /* decimating filter: up to 3 x taps */
#define RATE_ERROR_HOLD_US 5000000
#define LEARNED_MIN_RUN_US 20000000   /* trust the learned clock offset after 20 s in RUN */
#define STALE_ISR_US       (2 * I2S_IN_DMA_FRAMES * 1000000 / 48000)  /* two DMA blocks overdue */
#define RATE_MAX_PPM       600.0      /* controller range (+-500 ppm) plus measurement margin */

typedef enum {
    PIPE_NO_SIGNAL,
    PIPE_PRIME,
    PIPE_RUN,
    PIPE_RATE_ERROR,
} pipe_state_t;

static const char *const s_state_names[] = {"NO_SIGNAL", "PRIME", "RUN", "RATE_ERROR"};

static spsc_ring_t s_ring;
static int32_t s_ring_buf[RING_FRAMES * 2];
static int32_t s_in[IN_MAX_FRAMES * 2];
static int32_t s_mono[IN_MAX_FRAMES];
static resampler_t *s_rs;
static drift_ctrl_t s_dc;
static pipe_state_t s_st = PIPE_NO_SIGNAL;
static int64_t s_last_run_us;
static int64_t s_rate_error_since;
static uint32_t s_last_ovf_frames;
static volatile uint32_t s_underrun_v, s_ovf_v, s_lost_v;
static bool s_fade_in;
static int64_t s_run_since;
static double s_learned_ratio = 1.0;
static bool s_have_learned;
static rate_check_t s_rc;
static volatile float s_in_ppm;   /* measured input rate since the signal (re)appeared */
static volatile float s_ppm_now;  /* drift controller correction of the last RUN step */
static volatile uint32_t s_fill_now;

#if CONFIG_AURACAST_TEST_TONE
/* End-to-end check without a receiver: tone frequency per channel after resampling */
static int32_t s_prev[2];
#endif

static void set_state(pipe_state_t st)
{
    if (st != s_st) {
        ESP_LOGI(TAG, "state %s -> %s", s_state_names[s_st], s_state_names[st]);
        s_st = st;
    }
}

#if CONFIG_AURACAST_TEST_TONE
static void tone_ctl_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(CONFIG_AURACAST_TEST_TONE_START_DELAY_MS));
    ESP_ERROR_CHECK(test_tone_start(CONFIG_AURACAST_I2S_BCLK_GPIO, CONFIG_AURACAST_I2S_WS_GPIO,
                                    CONFIG_AURACAST_I2S_DIN_GPIO, CONFIG_AURACAST_TEST_TONE_RATE_HZ));
#if CONFIG_AURACAST_TEST_TONE_GAP_TEST
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ESP_LOGW(TAG, "gap test: test tone off for 2 s");
        test_tone_set_enabled(false);
        vTaskDelay(pdMS_TO_TICKS(2000));
        test_tone_set_enabled(true);
    }
#endif
    vTaskDelete(NULL);
}
#endif

static void source_init(void)
{
    spsc_ring_init(&s_ring, s_ring_buf, RING_FRAMES, 2);
    s_rs = resampler_create_decim(s_nch, s_out_frames, (uint8_t)(48000 / broadcast_bis_rate(0)));   /* 1, 2, 3 */
    ESP_ERROR_CHECK(s_rs ? ESP_OK : ESP_ERR_NO_MEM);
    const i2s_in_cfg_t cfg = {
        .bclk_gpio = CONFIG_AURACAST_I2S_BCLK_GPIO,
        .ws_gpio = CONFIG_AURACAST_I2S_WS_GPIO,
        .din_gpio = CONFIG_AURACAST_I2S_DIN_GPIO,
        .sample_rate_hz = 48000,
    };
    /* Called from the encoder task, so the DMA interrupt lives on core 1 as well */
    ESP_ERROR_CHECK(i2s_in_start(&cfg, &s_ring));
#if CONFIG_AURACAST_TEST_TONE
    xTaskCreatePinnedToCore(tone_ctl_task, "tone_ctl", 3072, NULL, 5, NULL, 0);
#endif
}

/* Resampler input for frames read into s_in: stereo as is, or one channel / the (L+R)/2 downmix for one BIS */
static const int32_t *mix_input(uint32_t frames)
{
    switch (s_channels) {
    case AURA_CH_STEREO:
        return s_in;
    case AURA_CH_MONO_LEFT:
    case AURA_CH_MONO_RIGHT: {
        const uint32_t off = s_channels == AURA_CH_MONO_RIGHT;
        for (uint32_t i = 0; i < frames; i++) {
            s_mono[i] = s_in[2 * i + off];
        }
        return s_mono;
    }
    default:
        for (uint32_t i = 0; i < frames; i++) {
            s_mono[i] = (s_in[2 * i] >> 1) + (s_in[2 * i + 1] >> 1);
        }
        return s_mono;
    }
}

/* Read and discard n frames (consumer side) */
static void ring_discard(uint32_t n)
{
    while (n > 0) {
        uint32_t chunk = n < IN_MAX_FRAMES ? n : IN_MAX_FRAMES;
        uint32_t got = spsc_ring_read(&s_ring, s_in, chunk);
        if (got == 0) {
            break;
        }
        n -= got;
    }
}

static void enter_run(int64_t now)
{
    const drift_ctrl_cfg_t dc_cfg = DRIFT_CTRL_CFG_DEFAULT(s_fifo_target);
    drift_ctrl_init(&s_dc, &dc_cfg);
    if (s_have_learned) {
        /* Warm start after a gap: the clock offset is a property of the hardware */
        drift_ctrl_preset(&s_dc, s_learned_ratio);
        ESP_LOGI(TAG, "warm start at %+.2f ppm", (s_learned_ratio - 1.0) * 1e6);
    }
    resampler_reset(s_rs);
    resampler_set_ratio(s_rs, drift_ctrl_ratio(&s_dc));

    /* Pre-load the resampler's look-ahead so its first pull takes a regular-sized block */
    uint32_t lookahead = resampler_input_needed(s_rs, s_out_frames) - FRAMES;
    lookahead = spsc_ring_read(&s_ring, s_in, lookahead);
    resampler_push(s_rs, mix_input(lookahead), lookahead);

    /* Then drop the excess so the control loop starts exactly at the set point (see test_drift_ctrl.c) */
    i2s_in_snapshot_t snap;
    i2s_in_snapshot(&snap);
    double est = fill_estimate(snap.ring_count, snap.t_last_isr_us, now, 48000.0, I2S_IN_DMA_FRAMES);
    double excess = floor(est - s_fifo_target);
    if (excess > 0) {
        ring_discard((uint32_t)excess);
    }
    s_run_since = now;
    s_last_run_us = now;
    s_fade_in = true;
    set_state(PIPE_RUN);
}

static void enter_rate_error(int64_t now, double ppm)
{
    ESP_LOGE(TAG, "input rate %+.0f ppm off 48 kHz (%.0f Hz), check FPGA sample rate", ppm,
             48000.0 * (1.0 + ppm * 1e-6));
    s_rate_error_since = now;
    s_have_learned = false;
    set_state(PIPE_RATE_ERROR);
}

/* One RUN step: control loop, resample one LC3 frame per channel into s_pcm. false = left RUN. */
static bool run_step(int64_t now, const i2s_in_snapshot_t *snap)
{
    double est = fill_estimate(snap->ring_count, snap->t_last_isr_us, now, 48000.0, I2S_IN_DMA_FRAMES);
    double dt = (double)(now - s_last_run_us) * 1e-6;
    dt = dt < 0.005 ? 0.005 : (dt > 0.05 ? 0.05 : dt);
    s_last_run_us = now;

    if (drift_ctrl_update(&s_dc, est, dt) == DRIFT_OUT_OF_RANGE) {
        /* Backstop: the direct rate check normally fires first */
        enter_rate_error(now, s_in_ppm);
        return false;
    }
    resampler_set_ratio(s_rs, drift_ctrl_ratio(&s_dc));
    double ppm = drift_ctrl_ppm(&s_dc);
    s_ppm_now = (float)ppm;
    s_fill_now = (uint32_t)(est > 0.0 ? est : 0.0);
    portENTER_CRITICAL(&s_win_lock);
    s_win.ppm_sum += ppm;
    s_win.fill_sum += est;
    s_win.ctrl_n++;
    portEXIT_CRITICAL(&s_win_lock);
    uint32_t need = resampler_input_needed(s_rs, s_out_frames);
    if (now - s_run_since >= LEARNED_MIN_RUN_US) {
        s_learned_ratio = drift_ctrl_learned_ratio(&s_dc);
        s_have_learned = true;
    }
    if (need > IN_MAX_FRAMES || spsc_ring_count(&s_ring) < need) {
        if (now - snap->t_last_isr_us > STALE_ISR_US) {
            /* The ring ran dry because the I2S clock stopped, not because we consumed too fast */
            s_lost_v++;
            set_state(PIPE_NO_SIGNAL);
        } else {
            s_underrun_v++;
            set_state(PIPE_PRIME);
        }
        return false;
    }
    spsc_ring_read(&s_ring, s_in, need);
    resampler_push(s_rs, mix_input(need), need);
    int32_t *const out[2] = {s_pcm[0], s_pcm[1]};
    resampler_pull(s_rs, out, s_out_frames);

    if (s_fade_in) {
        for (int ch = 0; ch < s_nch; ch++) {
            for (uint32_t i = 0; i < s_out_frames; i++) {
                s_pcm[ch][i] = (int32_t)((int64_t)s_pcm[ch][i] * i / s_out_frames);
            }
        }
        s_fade_in = false;
    }
#if CONFIG_AURACAST_TEST_TONE
    uint32_t cross[2] = {0, 0};
    for (int ch = 0; ch < s_nch; ch++) {
        for (uint32_t i = 0; i < s_out_frames; i++) {
            cross[ch] += (s_prev[ch] < 0 && s_pcm[ch][i] >= 0);
            s_prev[ch] = s_pcm[ch][i];
        }
    }
    portENTER_CRITICAL(&s_win_lock);
    s_win.cross[0] += cross[0];
    s_win.cross[1] += cross[1];
    s_win.cross_frames += s_out_frames;
    portEXIT_CRITICAL(&s_win_lock);
#endif
    return true;
}

static void source_fill(void)
{
    i2s_in_snapshot_t snap;
    i2s_in_snapshot(&snap);
    int64_t now = esp_timer_get_time();
    uint32_t ovf_delta = snap.overflow_frames - s_last_ovf_frames;
    s_last_ovf_frames = snap.overflow_frames;

    /* Direct rate check while a signal is present (reference restarts when the signal reappears) */
    rate_status_t rate = RATE_UNKNOWN;
    if (s_st == PIPE_PRIME || s_st == PIPE_RUN) {
        double ppm = 0.0;
        rate = rate_check_update(&s_rc, snap.frames_total, snap.t_last_isr_us, 48000.0, RATE_MAX_PPM, &ppm);
        s_in_ppm = (float)ppm;
        if (rate == RATE_BAD) {
            enter_rate_error(now, ppm);
            silence();
            return;
        }
    }

    switch (s_st) {
    case PIPE_NO_SIGNAL:
        if (snap.signal_present) {
            spsc_ring_drop_all(&s_ring);
            rate_check_reset(&s_rc);
            set_state(PIPE_PRIME);
        }
        break;
    case PIPE_PRIME:
        if (!snap.signal_present) {
            set_state(PIPE_NO_SIGNAL);
        } else if (rate == RATE_OK && snap.ring_count >= s_fifo_target) {
            enter_run(now);
            /* Consume in this tick already, otherwise the loop starts 480 frames above the set point */
            i2s_in_snapshot(&snap);
            if (run_step(now, &snap)) {
                return;
            }
        }
        break;
    case PIPE_RUN:
        if (!snap.signal_present) {
            s_lost_v++;
            set_state(PIPE_NO_SIGNAL);
        } else if (ovf_delta) {
            s_ovf_v += ovf_delta;
            set_state(PIPE_PRIME);
        } else if (run_step(now, &snap)) {
            return;
        }
        break;
    case PIPE_RATE_ERROR:
        if (now - s_rate_error_since > RATE_ERROR_HOLD_US) {
            set_state(PIPE_NO_SIGNAL);
        }
        break;
    }
    silence();
}

static const char *source_state(void)
{
    return s_state_names[s_st];
}

/* Stats task: fields of the status line that belong to the I2S source */
static void source_log_fields(const window_stats_t *w, double *fill, double *ppm, double *in_ppm,
                              uint32_t *underrun, uint32_t *ovf, uint32_t *lost)
{
    *in_ppm = s_in_ppm;
    *fill = w->ctrl_n ? w->fill_sum / w->ctrl_n : 0.0;
    *ppm = w->ctrl_n ? w->ppm_sum / w->ctrl_n : 0.0;
    *underrun = s_underrun_v;
    *ovf = s_ovf_v;
    *lost = s_lost_v;
#if CONFIG_AURACAST_TEST_TONE
    if (w->cross_frames) {
        double secs = w->cross_frames / (double)broadcast_bis_rate(0);
        ESP_LOGI(TAG, "tone check (after resampler): L=%.1fHz R=%.1fHz", w->cross[0] / secs, w->cross[1] / secs);
    }
#endif
}
#endif /* CONFIG_AURACAST_SOURCE_I2S */

static uint32_t bits_take(int who)
{
    uint32_t b = 0;
#if CONFIG_AURACAST_SOURCE_I2S
    b = i2s_in_take_bit_mask();
#endif
    portENTER_CRITICAL(&s_bits_lock);
    s_bits_acc[BITS_LOG] |= b;
    s_bits_acc[BITS_STATUS] |= b;
    uint32_t r = s_bits_acc[who];
    s_bits_acc[who] = 0;
    portEXIT_CRITICAL(&s_bits_lock);
    return r;
}

void pipeline_get_status(pipeline_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_source_ready) {
        return;
    }
#if CONFIG_AURACAST_SOURCE_I2S
    _Static_assert(PIPE_NO_SIGNAL == AURA_PIPE_NO_SIGNAL && PIPE_PRIME == AURA_PIPE_PRIME && PIPE_RUN == AURA_PIPE_RUN &&
                   PIPE_RATE_ERROR == AURA_PIPE_RATE_ERROR, "pipe states match the protocol");
    i2s_in_snapshot_t snap;
    i2s_in_snapshot(&snap);
    out->state = (uint8_t)s_st;
    out->signal = snap.signal_present;
    out->fifo_fill = s_st == PIPE_RUN ? s_fill_now : snap.ring_count;
    out->drift_ppm = s_ppm_now;
    out->in_ppm = s_in_ppm;
    out->underrun = s_underrun_v;
    out->overflow = s_ovf_v;
    out->lost = s_lost_v;
#else
    out->state = AURA_PIPE_RUN;
#endif
    out->i2s_bits = bits_take(BITS_STATUS);
}

static void produce_frame(void)
{
    int64_t t0 = esp_timer_get_time();
    source_fill();
    bool ok = true;
    for (int ch = 0; ch < s_nch; ch++) {
        ok = encode(ch) && ok;
    }
    s_frames_made++;
    if (!ok) {
        ESP_LOGE(TAG, "LC3 encode failed");
    }
#if CONFIG_AURACAST_DEBUG_SKIP_SEND_EVERY > 0
    if (s_frames_made % CONFIG_AURACAST_DEBUG_SKIP_SEND_EVERY == 0) {
        return;
    }
#endif
    const uint8_t *const sdu[2] = {s_sdu[0], s_sdu[1]};
    const uint16_t len[2] = {s_sdu_len, s_sdu_len};
    broadcast_send_frame(sdu, len);
    uint32_t proc = (uint32_t)(esp_timer_get_time() - t0);
    portENTER_CRITICAL(&s_win_lock);
    s_win.proc_us_max = proc > s_win.proc_us_max ? proc : s_win.proc_us_max;
    portEXIT_CRITICAL(&s_win_lock);
}

static void stats_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(STATS_PERIOD_US / 1000));
        window_stats_t w;
        portENTER_CRITICAL(&s_win_lock);
        w = s_win;
        memset(&s_win, 0, sizeof(s_win));
        portEXIT_CRITICAL(&s_win_lock);

        broadcast_stats_t st;
        double fill, ppm, in_ppm;
        uint32_t underrun, ovf, lost;
        broadcast_get_stats(&st, true);
        source_log_fields(&w, &fill, &ppm, &in_ppm, &underrun, &ovf, &lost);
        ESP_LOGI(TAG, "state=%s fill=%.1f ppm=%+.2f in_ppm=%+.2f underrun=%lu ovf=%lu lost=%lu senderr=%lu wd=%lu "
                 "tick_us=[%lu..%lu] mean=%.1f proc_us_max=%lu sent=%lu heap=%lu bt_ppm=%+.2f",
                 source_state(), fill, ppm, in_ppm, (unsigned long)underrun, (unsigned long)ovf,
                 (unsigned long)lost, (unsigned long)st.send_err, (unsigned long)st.wd_refills,
                 (unsigned long)st.tick_us_min, (unsigned long)st.tick_us_max, st.tick_us_mean,
                 (unsigned long)w.proc_us_max, (unsigned long)st.sent, (unsigned long)esp_get_free_heap_size(),
                 st.bt_ppm);
#if CONFIG_AURACAST_SOURCE_I2S
        ESP_LOGI(TAG, "i2s bits=0x%08lx", (unsigned long)bits_take(BITS_LOG));
#endif
    }
}

static void encoder_task(void *arg)
{
    s_sdu_len = broadcast_bis_sdu_len(0);
    s_nch = broadcast_stream_count();
    s_out_frames = broadcast_bis_rate(0) / 100;
    if (s_sdu_len > MAX_SDU || encoders_open() != ESP_OK) {
        ESP_LOGE(TAG, "encoder setup failed (sdu %u)", s_sdu_len);
        vTaskDelete(NULL);
        return;
    }
    source_init();
    s_source_ready = true;
    s_task = xTaskGetCurrentTaskHandle();

    for (;;) {
        uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WATCHDOG_MS));
        if (notified == 0 && broadcast_streaming()) {
            /* No confirmation for 50 ms: assume the controller queue ran dry and refill it */
            broadcast_reset_in_flight();
        }
        for (uint32_t n = broadcast_frames_wanted(); n > 0; n--) {
            produce_frame();
        }
    }
}

esp_err_t pipeline_start(const ctrl_config_t *cfg)
{
    s_channels = cfg->channels;
    s_fifo_target = cfg->fifo_target;
    if (xTaskCreatePinnedToCore(encoder_task, "encoder", 6144, NULL, 15, NULL, 1) != pdPASS ||
        xTaskCreatePinnedToCore(stats_task, "stats", 4096, NULL, 2, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

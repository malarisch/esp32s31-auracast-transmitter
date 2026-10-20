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
 * One subgroup: the channel mix happens before the resampler, which also decimates to the BIS rate.
 * Two subgroups (SQ + HQ): the drift-controlled resampler runs at 48 kHz stereo; the HQ BIS take their
 * channels from that, the SQ BIS go through a fixed FIR decimator (2:1 or 3:1, components/audio_dsp).
 *
 *   NO_SIGNAL --signal--> PRIME --fill >= set point--> RUN
 *   RUN --signal lost--> NO_SIGNAL, RUN --underrun/overflow--> PRIME,
 *   RUN --ratio pinned at the clamp--> RATE_ERROR --5 s--> NO_SIGNAL
 * Outside RUN, encoded silence keeps the broadcast alive.
 */
#include <math.h>
#include <string.h>

#include "esp_lc3_enc.h"
#include "esp_heap_caps.h"
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
#include "decimator.h"
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

typedef struct {
    void *enc;                  /* LC3 encoder */
    int32_t pcm[FRAMES];        /* one LC3 frame at the BIS rate */
    uint8_t sdu[MAX_SDU];
    uint16_t sdu_len;
    uint32_t out_frames;        /* LC3 frame length at the BIS rate: rate / 100 */
} bis_t;

static TaskHandle_t s_task;
static bis_t s_bis[BROADCAST_MAX_BIS];
static uint8_t s_nbis;
static bool s_dual;                        /* two subgroups */
static const broadcast_subgroup_t *s_sg[2]; /* [0] = SQ or the only one, [1] = HQ */
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
    uint64_t proc_us_sum;
    uint32_t proc_n;
    uint32_t pcm_peak[BROADCAST_MAX_BIS];   /* largest |sample| fed to each encoder */
    uint32_t zero_sdus;                     /* SDUs sent whose bytes were all zero (LC3 silence) */
    uint32_t late;                          /* SDU sets whose processing took longer than the 10 ms interval */
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
    for (int i = 0; i < s_nbis; i++) {
        bis_t *b = &s_bis[i];
        b->sdu_len = broadcast_bis_sdu_len(i);
        b->out_frames = broadcast_bis_rate(i) / 100;
        if (b->sdu_len > MAX_SDU || b->out_frames > FRAMES || b->out_frames == 0) {
            ESP_LOGE(TAG, "BIS %d: unsupported preset (sdu %u, %lu frames)", i, b->sdu_len, (unsigned long)b->out_frames);
            return ESP_FAIL;
        }
        esp_lc3_enc_config_t cfg = ESP_LC3_ENC_CONFIG_DEFAULT();
        cfg.sample_rate = broadcast_bis_rate(i);
        cfg.bits_per_sample = 32;
        cfg.channel = 1;
        cfg.frame_dms = 100;
        cfg.nbyte = b->sdu_len;
        if (esp_lc3_enc_open(&cfg, sizeof(cfg), &b->enc) != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "LC3 encoder %d open failed", i);
            return ESP_FAIL;
        }
        int in_size = 0, out_size = 0;
        esp_lc3_enc_get_frame_size(b->enc, &in_size, &out_size);
        ESP_LOGI(TAG, "LC3 encoder %d: %lu Hz, in %d bytes, out %d bytes per frame, free heap %lu", i,
                 (unsigned long)broadcast_bis_rate(i), in_size, out_size, (unsigned long)esp_get_free_heap_size());
        if (in_size != (int)(b->out_frames * sizeof(int32_t))) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static bool encode(int i)
{
    bis_t *b = &s_bis[i];
    esp_audio_enc_in_frame_t in = {
        .buffer = (uint8_t *)b->pcm,
        .len = b->out_frames * sizeof(int32_t),
    };
    esp_audio_enc_out_frame_t out = {
        .buffer = b->sdu,
        .len = b->sdu_len,
    };
    return esp_lc3_enc_process(b->enc, &in, &out) == ESP_AUDIO_ERR_OK && out.encoded_bytes == b->sdu_len;
}

static void silence(void)
{
    for (int i = 0; i < s_nbis; i++) {
        memset(s_bis[i].pcm, 0, sizeof(s_bis[i].pcm));
    }
}

#if CONFIG_AURACAST_SOURCE_SYNTH
/*
 * 25 ms hold exactly 11 periods of 440 Hz and 22 of 880 Hz (1200/600/400 frames at 48/24/16 kHz);
 * computed once (double is soft-float here)
 */
#define SYNTH_TABLE_FRAMES 1200
static int32_t s_synth[BROADCAST_MAX_BIS][SYNTH_TABLE_FRAMES];
static uint32_t s_synth_len[BROADCAST_MAX_BIS];

static void source_init(void)
{
    static const double freq[2] = {440.0, 880.0};
    for (int i = 0; i < s_nbis; i++) {
        const uint32_t rate = broadcast_bis_rate(i);
        /* 440 Hz on the first BIS of a subgroup, 880 Hz on its second (right) one */
        const broadcast_subgroup_t *sg = s_dual && i >= s_sg[1]->first_bis ? s_sg[1] : s_sg[0];
        const bool second = i == sg->first_bis + 1;
        s_synth_len[i] = rate / 40;
        for (uint32_t k = 0; k < s_synth_len[i]; k++) {
            s_synth[i][k] = (int32_t)lrint(SYNTH_AMPLITUDE * 2147483647.0 * sin(2.0 * M_PI * freq[second] * k / rate));
        }
    }
}

static void source_fill(void)
{
    static uint32_t pos[BROADCAST_MAX_BIS];
    for (int i = 0; i < s_nbis; i++) {
        for (uint32_t k = 0; k < s_bis[i].out_frames; k++) {
            s_bis[i].pcm[k] = s_synth[i][pos[i]];
            pos[i] = (pos[i] + 1) % s_synth_len[i];
        }
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
static resampler_t *s_rs;            /* drift-controlled: to the BIS rate (one subgroup) or 48 kHz stereo (two) */
static uint32_t s_rs_out_frames;     /* frames per pull from s_rs */
static int32_t s_pcm48[2][FRAMES];   /* two subgroups: 48 kHz stereo after the drift correction */
static decimator_t *s_dec_sq;        /* two subgroups: fixed 2:1 / 3:1 decimator for the SQ BIS */
static int32_t s_sq_in[2 * FRAMES];  /* interleaved input of s_dec_sq */
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
static int32_t s_prev[BROADCAST_MAX_BIS];
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
    if (s_dual) {
        s_rs_out_frames = FRAMES;
        s_rs = resampler_create_decim(2, FRAMES, 1);
        s_dec_sq = decimator_create(s_sg[0]->bis_count, (uint8_t)(48000 / s_sg[0]->rate), FRAMES);
        ESP_ERROR_CHECK(s_rs && s_dec_sq ? ESP_OK : ESP_ERR_NO_MEM);
        if (s_nbis == BROADCAST_MAX_BIS && s_sg[1]->sdu_len >= 155) {
            ESP_LOGW(TAG, "4 BIS with 48_6_1: encoder load close to the 10 ms frame (measured mean 7.7 ms, max up to 9.7 ms); "
                     "watch late= in the status line, prefer 48_4_1 or a mono SQ subgroup");
        }
    } else {
        s_rs_out_frames = s_bis[0].out_frames;
        s_rs = resampler_create_decim(s_nbis, s_rs_out_frames, (uint8_t)(48000 / s_sg[0]->rate));   /* 1, 2, 3 */
        ESP_ERROR_CHECK(s_rs ? ESP_OK : ESP_ERR_NO_MEM);
    }
    const i2s_in_cfg_t cfg = {
        .bclk_gpio = CONFIG_AURACAST_I2S_BCLK_GPIO,
        .ws_gpio = CONFIG_AURACAST_I2S_WS_GPIO,
        .din_gpio = CONFIG_AURACAST_I2S_DIN_GPIO,
        .sample_rate_hz = 48000,
    };
    /* Called from the encoder task, so the DMA interrupt lives on core 1 as well */
    ESP_LOGI(TAG, "before I2S start: free heap %lu, DMA-capable %lu (largest block %lu)",
             (unsigned long)esp_get_free_heap_size(), (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    ESP_ERROR_CHECK(i2s_in_start(&cfg, &s_ring));
#if CONFIG_AURACAST_TEST_TONE
    xTaskCreatePinnedToCore(tone_ctl_task, "tone_ctl", 3072, NULL, 5, NULL, 0);
#endif
}

/*
 * Channel mode applied to planar 48 kHz stereo: stereo copies both channels, the mono modes write one.
 * out0/out1 are written with `stride` (1 = planar, 2 = interleaved).
 */
static void derive(uint8_t channels, const int32_t *const in48[2], uint32_t frames, int32_t *out0, int32_t *out1,
                   uint32_t stride)
{
    for (uint32_t i = 0; i < frames; i++) {
        int32_t v;
        switch (channels) {
        case AURA_CH_STEREO:
            out0[i * stride] = in48[0][i];
            out1[i * stride] = in48[1][i];
            continue;
        case AURA_CH_MONO_LEFT:
            v = in48[0][i];
            break;
        case AURA_CH_MONO_RIGHT:
            v = in48[1][i];
            break;
        default:
            v = (in48[0][i] >> 1) + (in48[1][i] >> 1);
            break;
        }
        out0[i * stride] = v;
    }
}

/*
 * Resampler input for frames read into s_in: stereo as is, or one channel / the (L+R)/2 downmix for one
 * BIS (one subgroup). With two subgroups the resampler always takes the stereo input.
 */
static const int32_t *mix_input(uint32_t frames)
{
    switch (s_dual ? AURA_CH_STEREO : s_sg[0]->channels) {
    case AURA_CH_STEREO:
        return s_in;
    case AURA_CH_MONO_LEFT:
    case AURA_CH_MONO_RIGHT: {
        const uint32_t off = s_sg[0]->channels == AURA_CH_MONO_RIGHT;
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
    uint32_t lookahead = resampler_input_needed(s_rs, s_rs_out_frames) - FRAMES;
    lookahead = spsc_ring_read(&s_ring, s_in, lookahead);
    resampler_push(s_rs, mix_input(lookahead), lookahead);
    if (s_dual) {
        decimator_reset(s_dec_sq);
    }

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

/* One RUN step: control loop, resample one LC3 frame per BIS into s_bis[].pcm. false = left RUN. */
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
    uint32_t need = resampler_input_needed(s_rs, s_rs_out_frames);
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
    if (s_dual) {
        int32_t *const out48[2] = {s_pcm48[0], s_pcm48[1]};
        resampler_pull(s_rs, out48, FRAMES);
        const int32_t *const in48[2] = {s_pcm48[0], s_pcm48[1]};
        /* HQ BIS: straight from the 48 kHz output */
        derive(s_sg[1]->channels, in48, FRAMES, s_bis[s_sg[1]->first_bis].pcm,
               s_bis[s_sg[1]->first_bis + 1].pcm, 1);
        /* SQ BIS: channel mode, then the fixed decimator */
        const uint32_t sq_nch = s_sg[0]->bis_count;
        derive(s_sg[0]->channels, in48, FRAMES, s_sq_in, s_sq_in + 1, sq_nch);
        int32_t *const out_sq[2] = {s_bis[s_sg[0]->first_bis].pcm, s_bis[s_sg[0]->first_bis + 1].pcm};
        decimator_process(s_dec_sq, s_sq_in, FRAMES, out_sq);
    } else {
        int32_t *const out[2] = {s_bis[0].pcm, s_bis[1].pcm};
        resampler_pull(s_rs, out, s_rs_out_frames);
    }

    if (s_fade_in) {
        for (int b = 0; b < s_nbis; b++) {
            const uint32_t n = s_bis[b].out_frames;
            for (uint32_t i = 0; i < n; i++) {
                s_bis[b].pcm[i] = (int32_t)((int64_t)s_bis[b].pcm[i] * i / n);
            }
        }
        s_fade_in = false;
    }
#if CONFIG_AURACAST_TEST_TONE
    uint32_t cross[2] = {0, 0};
    for (int b = 0; b < s_nbis && b < 2; b++) {
        for (uint32_t i = 0; i < s_bis[b].out_frames; i++) {
            cross[b] += (s_prev[b] < 0 && s_bis[b].pcm[i] >= 0);
            s_prev[b] = s_bis[b].pcm[i];
        }
    }
    portENTER_CRITICAL(&s_win_lock);
    s_win.cross[0] += cross[0];
    s_win.cross[1] += cross[1];
    s_win.cross_frames += s_bis[0].out_frames;
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
    for (int i = 0; i < s_nbis; i++) {
        ok = encode(i) && ok;
    }
    s_frames_made++;
    if (!ok) {
        ESP_LOGE(TAG, "LC3 encode failed");
    }
    uint32_t peak[BROADCAST_MAX_BIS] = {0};
    uint32_t zero = 0;
    for (int i = 0; i < s_nbis; i++) {
        for (uint32_t k = 0; k < s_bis[i].out_frames; k++) {
            const int32_t v = s_bis[i].pcm[k];
            const uint32_t a = v < 0 ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
            peak[i] = a > peak[i] ? a : peak[i];
        }
        bool nonzero = false;
        for (uint32_t k = 0; k < s_bis[i].sdu_len && !nonzero; k++) {
            nonzero = s_bis[i].sdu[k] != 0;
        }
        zero += !nonzero;
    }
    portENTER_CRITICAL(&s_win_lock);
    for (int i = 0; i < s_nbis; i++) {
        s_win.pcm_peak[i] = peak[i] > s_win.pcm_peak[i] ? peak[i] : s_win.pcm_peak[i];
    }
    s_win.zero_sdus += zero;
    portEXIT_CRITICAL(&s_win_lock);
#if CONFIG_AURACAST_DEBUG_SKIP_SEND_EVERY > 0
    if (s_frames_made % CONFIG_AURACAST_DEBUG_SKIP_SEND_EVERY == 0) {
        return;
    }
#endif
    const uint8_t *sdu[BROADCAST_MAX_BIS];
    uint16_t len[BROADCAST_MAX_BIS];
    for (int i = 0; i < s_nbis; i++) {
        sdu[i] = s_bis[i].sdu;
        len[i] = s_bis[i].sdu_len;
    }
    broadcast_send_frame(sdu, len);
    uint32_t proc = (uint32_t)(esp_timer_get_time() - t0);
    portENTER_CRITICAL(&s_win_lock);
    s_win.proc_us_max = proc > s_win.proc_us_max ? proc : s_win.proc_us_max;
    s_win.proc_us_sum += proc;
    s_win.proc_n++;
    s_win.late += proc > 10000;
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
                 "tick_us=[%lu..%lu] mean=%.1f proc_us_max=%lu proc_us_mean=%lu late=%lu sent=%lu heap=%lu bt_ppm=%+.2f",
                 source_state(), fill, ppm, in_ppm, (unsigned long)underrun, (unsigned long)ovf,
                 (unsigned long)lost, (unsigned long)st.send_err, (unsigned long)st.wd_refills,
                 (unsigned long)st.tick_us_min, (unsigned long)st.tick_us_max, st.tick_us_mean,
                 (unsigned long)w.proc_us_max, (unsigned long)(w.proc_n ? w.proc_us_sum / w.proc_n : 0),
                 (unsigned long)w.late, (unsigned long)st.sent, (unsigned long)esp_get_free_heap_size(), st.bt_ppm);
        ESP_LOGI(TAG, "zero_sdus=%lu pcm peak dBFS: %.1f %.1f %.1f %.1f", (unsigned long)w.zero_sdus,
                 20.0 * log10((double)w.pcm_peak[0] / 2147483648.0 + 1e-12), 20.0 * log10((double)w.pcm_peak[1] / 2147483648.0 + 1e-12),
                 20.0 * log10((double)w.pcm_peak[2] / 2147483648.0 + 1e-12), 20.0 * log10((double)w.pcm_peak[3] / 2147483648.0 + 1e-12));
#if CONFIG_AURACAST_SOURCE_I2S
        ESP_LOGI(TAG, "i2s bits=0x%08lx", (unsigned long)bits_take(BITS_LOG));
#endif
    }
}

static void encoder_task(void *arg)
{
    s_nbis = broadcast_stream_count();
    s_dual = broadcast_subgroup_count() == 2;
    s_sg[0] = broadcast_subgroup(0);
    s_sg[1] = broadcast_subgroup(1);
    if (s_nbis == 0 || s_nbis > BROADCAST_MAX_BIS || s_sg[0] == NULL || (s_dual && s_sg[1] == NULL) ||
        encoders_open() != ESP_OK) {
        ESP_LOGE(TAG, "encoder setup failed (%u BIS)", s_nbis);
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
    s_fifo_target = cfg->fifo_target;
    if (xTaskCreatePinnedToCore(encoder_task, "encoder", 6144, NULL, 15, NULL, 1) != pdPASS ||
        xTaskCreatePinnedToCore(stats_task, "stats", 4096, NULL, 2, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

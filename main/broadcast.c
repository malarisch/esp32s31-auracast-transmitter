/*
 * SPDX-FileCopyrightText: 2021-2022 Nordic Semiconductor ASA
 * SPDX-FileContributor: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * Source setup derived from the ESP-IDF example
 * examples/bluetooth/esp_ble_audio/bap/broadcast_source/main/main.c.
 *
 * Auracast (BAP broadcast source + Public Broadcast Profile announcement) with two BIS
 * (front left / front right). Pacing: keep BROADCAST_TX_DEPTH SDUs queued per BIS. The number
 * in flight is derived from sequence numbers (next to send vs. newest completed on BIS 0), so a
 * lost or late completion callback corrects itself at the next one.
 */
#include <stdatomic.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "esp_ble_audio_bap_api.h"
#include "esp_ble_audio_bap_lc3_preset_defs.h"
#include "esp_ble_audio_common_api.h"
#include "esp_ble_audio_lc3_defs.h"
#include "esp_ble_audio_pbp_api.h"
#include "esp_ble_iso_common_api.h"

#include "ble_audio_example_utils.h"

#include "tx_pacing.h"

#include "adv.h"
#include "broadcast.h"

static const char *TAG = "broadcast";

#define LOCATIONS (ESP_BLE_AUDIO_LOCATION_FRONT_LEFT | ESP_BLE_AUDIO_LOCATION_FRONT_RIGHT)

#if CONFIG_AURACAST_PRESET_48_2_1
ESP_BLE_AUDIO_BAP_LC3_BROADCAST_PRESET_48_2_1_DEFINE(s_preset, LOCATIONS, ESP_BLE_AUDIO_CONTEXT_TYPE_MEDIA);
#define PRESET_NAME "48_2_1"
#elif CONFIG_AURACAST_PRESET_48_6_1
ESP_BLE_AUDIO_BAP_LC3_BROADCAST_PRESET_48_6_1_DEFINE(s_preset, LOCATIONS, ESP_BLE_AUDIO_CONTEXT_TYPE_MEDIA);
#define PRESET_NAME "48_6_1"
#else
ESP_BLE_AUDIO_BAP_LC3_BROADCAST_PRESET_48_4_1_DEFINE(s_preset, LOCATIONS, ESP_BLE_AUDIO_CONTEXT_TYPE_MEDIA);
#define PRESET_NAME "48_4_1"
#endif

#define STREAM_COUNT 2
_Static_assert(CONFIG_BT_BAP_BROADCAST_SRC_STREAM_COUNT >= STREAM_COUNT, "need 2 broadcast streams");
/* Broadcast_Name must be 4..32 bytes (PBP); the code at most 16. Both are copied into fixed buffers. */
_Static_assert(sizeof(CONFIG_AURACAST_BROADCAST_NAME) - 1 >= 4 && sizeof(CONFIG_AURACAST_BROADCAST_NAME) - 1 <= 32,
               "AURACAST_BROADCAST_NAME must be 4..32 bytes");
_Static_assert(sizeof(CONFIG_AURACAST_BROADCAST_CODE) - 1 <= 16, "AURACAST_BROADCAST_CODE must be at most 16 bytes");

static struct {
    esp_ble_audio_bap_stream_t stream;
    atomic_bool started;
} s_streams[STREAM_COUNT];

static esp_ble_audio_bap_broadcast_source_t *s_source;
static broadcast_tick_cb_t s_tick;
static void *s_tick_arg;
static atomic_bool s_streaming;
static uint16_t s_seq;                 /* next sequence number; written by the encoder task only */
static atomic_uint s_last_done;        /* newest completed sequence number on BIS 0 */
static uint32_t s_broadcast_id;

/* Statistics (tick window written from the ISO task, read under a short critical section) */
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static atomic_uint s_sent, s_send_err, s_wd_refills;
static int64_t s_last_tick_us;
static uint32_t s_tick_min = UINT32_MAX, s_tick_max;
static uint64_t s_tick_sum;
static uint32_t s_tick_n;
static int64_t s_first_tick_us;
static uint32_t s_ticks_total;

static int stream_index(const esp_ble_audio_bap_stream_t *stream)
{
    for (int i = 0; i < STREAM_COUNT; i++) {
        if (&s_streams[i].stream == stream) {
            return i;
        }
    }
    return -1;
}

static void stream_started_cb(esp_ble_audio_bap_stream_t *stream)
{
    int idx = stream_index(stream);
    ESP_LOGI(TAG, "BIS %d streaming", idx);
    if (idx < 0) {
        return;
    }
    atomic_store(&s_streams[idx].started, true);
    if (atomic_load(&s_streams[0].started) && atomic_load(&s_streams[1].started)) {
        atomic_store(&s_last_done, (uint16_t)(s_seq - 1u));
        s_last_tick_us = 0;
        s_first_tick_us = 0;
        s_ticks_total = 0;
        atomic_store(&s_streaming, true);
        s_tick(s_tick_arg);   /* prime the controller queue */
    }
}

static void stream_ended(esp_ble_audio_bap_stream_t *stream, const char *what, uint8_t reason)
{
    int idx = stream_index(stream);
    ESP_LOGW(TAG, "BIS %d %s, reason 0x%02x", idx, what, reason);
    if (idx >= 0) {
        atomic_store(&s_streams[idx].started, false);
    }
    atomic_store(&s_streaming, false);
}

static void stream_stopped_cb(esp_ble_audio_bap_stream_t *stream, uint8_t reason)
{
    stream_ended(stream, "stopped", reason);
}

static void stream_disconnected_cb(esp_ble_audio_bap_stream_t *stream, uint8_t reason)
{
    stream_ended(stream, "ISO disconnected", reason);
}

static void stream_sent_cb(esp_ble_audio_bap_stream_t *stream, void *user_data)
{
    if (stream_index(stream) != 0) {
        return;
    }
#if CONFIG_AURACAST_DEBUG_DROP_SENT_EVERY > 0
    static uint32_t s_cb_count;
    if (++s_cb_count % CONFIG_AURACAST_DEBUG_DROP_SENT_EVERY == 0) {
        return; /* emulate a completion event lost in the stack */
    }
#endif
    const esp_ble_iso_tx_cb_info_t *info = user_data;
    uint32_t completed = 1;
    if (info != NULL && info->pkt_cnt > 0) {
        atomic_store(&s_last_done, info->pkt[info->pkt_cnt - 1].seq_num);
        completed = info->completed_pkts ? info->completed_pkts : 1;
    } else {
        /* No sequence information: count the completions instead */
        completed = (info != NULL && info->completed_pkts) ? info->completed_pkts : 1;
        atomic_store(&s_last_done, (uint16_t)(atomic_load(&s_last_done) + completed));
    }

    atomic_fetch_add(&s_sent, completed);
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_stats_lock);
    if (s_last_tick_us) {
        uint32_t dt = (uint32_t)(now - s_last_tick_us);
        s_tick_min = dt < s_tick_min ? dt : s_tick_min;
        s_tick_max = dt > s_tick_max ? dt : s_tick_max;
        s_tick_sum += dt;
        s_tick_n++;
    }
    s_last_tick_us = now;
    if (s_first_tick_us == 0) {
        s_first_tick_us = now;
    }
    s_ticks_total++;
    portEXIT_CRITICAL(&s_stats_lock);

    s_tick(s_tick_arg);
}

static esp_ble_audio_bap_stream_ops_t s_stream_ops = {
    .started = stream_started_cb,
    .stopped = stream_stopped_cb,
    .sent = stream_sent_cb,
    .disconnected = stream_disconnected_cb,
};

static void source_started_cb(esp_ble_audio_bap_broadcast_source_t *source)
{
    ESP_LOGI(TAG, "broadcast source started");
}

static void source_stopped_cb(esp_ble_audio_bap_broadcast_source_t *source, uint8_t reason)
{
    ESP_LOGW(TAG, "broadcast source stopped, reason 0x%02x", reason);
    atomic_store(&s_streaming, false);
}

static bool encrypted(void)
{
    return strlen(CONFIG_AURACAST_BROADCAST_CODE) > 0;
}

static esp_err_t source_create(void)
{
    static uint8_t chan_left[] = {
        ESP_BLE_AUDIO_CODEC_DATA(ESP_BLE_AUDIO_CODEC_CFG_CHAN_ALLOC,
                                 EXAMPLE_BYTES_LIST_LE32(ESP_BLE_AUDIO_LOCATION_FRONT_LEFT))
    };
    static uint8_t chan_right[] = {
        ESP_BLE_AUDIO_CODEC_DATA(ESP_BLE_AUDIO_CODEC_CFG_CHAN_ALLOC,
                                 EXAMPLE_BYTES_LIST_LE32(ESP_BLE_AUDIO_LOCATION_FRONT_RIGHT))
    };
    static esp_ble_audio_bap_broadcast_source_cb_t source_cb = {
        .started = source_started_cb,
        .stopped = source_stopped_cb,
    };
    esp_ble_audio_bap_broadcast_source_stream_param_t stream_params[STREAM_COUNT];
    esp_ble_audio_bap_broadcast_source_subgroup_param_t subgroup = {0};
    esp_ble_audio_bap_broadcast_source_param_t create = {0};

    esp_err_t err = esp_ble_audio_bap_broadcast_source_register_cb(&source_cb);
    if (err) {
        ESP_LOGE(TAG, "register source callbacks failed, err %d", err);
        return err;
    }

    for (int i = 0; i < STREAM_COUNT; i++) {
        stream_params[i].stream = &s_streams[i].stream;
        stream_params[i].data = i == 0 ? chan_left : chan_right;
        stream_params[i].data_len = i == 0 ? sizeof(chan_left) : sizeof(chan_right);
        esp_ble_audio_bap_stream_cb_register(&s_streams[i].stream, &s_stream_ops);
    }
    subgroup.params_count = STREAM_COUNT;
    subgroup.params = stream_params;
    subgroup.codec_cfg = &s_preset.codec_cfg;

    create.params_count = 1;
    create.params = &subgroup;
    create.qos = &s_preset.qos;
    create.packing = ESP_BLE_ISO_PACKING_SEQUENTIAL;
    create.encryption = encrypted();
    if (create.encryption) {
        static const char code[] = CONFIG_AURACAST_BROADCAST_CODE;
        size_t n = sizeof(code) - 1;
        memcpy(create.broadcast_code, code, n < sizeof(create.broadcast_code) ? n : sizeof(create.broadcast_code));
    }

    err = esp_ble_audio_bap_broadcast_source_create(&create, &s_source);
    if (err) {
        ESP_LOGE(TAG, "create broadcast source failed, err %d", err);
    }
    return err;
}

/*
 * Extended advertising payload:
 *   Service Data 0x1852 (Broadcast Audio Announcement) + Broadcast_ID
 *   Service Data 0x1856 (Public Broadcast Announcement, from the PBP API)
 *   Broadcast_Name
 */
static uint8_t build_ext_adv(uint8_t *buf, size_t cap)
{
    size_t len = 0;
    const char *name = CONFIG_AURACAST_BROADCAST_NAME;
    size_t name_len = strlen(name);

    buf[len++] = 6;
    buf[len++] = EXAMPLE_AD_TYPE_SERVICE_DATA16;
    buf[len++] = ESP_BLE_AUDIO_UUID_BROADCAST_AUDIO_VAL & 0xFF;
    buf[len++] = (ESP_BLE_AUDIO_UUID_BROADCAST_AUDIO_VAL >> 8) & 0xFF;
    buf[len++] = s_broadcast_id & 0xFF;
    buf[len++] = (s_broadcast_id >> 8) & 0xFF;
    buf[len++] = (s_broadcast_id >> 16) & 0xFF;

    NET_BUF_SIMPLE_DEFINE(pba, ESP_BLE_AUDIO_PBP_MIN_PBA_SIZE);
    esp_ble_audio_pbp_announcement_feature_t features = ESP_BLE_AUDIO_PBP_ANNOUNCEMENT_FEATURE_HIGH_QUALITY;
    if (encrypted()) {
        features |= ESP_BLE_AUDIO_PBP_ANNOUNCEMENT_FEATURE_ENCRYPTION;
    }
    if (esp_ble_audio_pbp_get_announcement(NULL, 0, features, &pba) != ESP_OK) {
        ESP_LOGE(TAG, "PBP announcement failed");
        return 0;
    }
    buf[len++] = 1 + pba.len;
    buf[len++] = EXAMPLE_AD_TYPE_SERVICE_DATA16;
    memcpy(buf + len, pba.data, pba.len);
    len += pba.len;

    buf[len++] = 1 + name_len;
    buf[len++] = EXAMPLE_AD_TYPE_BROADCAST_NAME;
    memcpy(buf + len, name, name_len);
    len += name_len;

    return len <= cap ? (uint8_t)len : 0;
}

static uint8_t build_per_adv(uint8_t *buf, size_t cap)
{
    NET_BUF_SIMPLE_DEFINE(base, 128);
    if (esp_ble_audio_bap_broadcast_source_get_base(s_source, &base) != ESP_OK) {
        ESP_LOGE(TAG, "encode BASE failed");
        return 0;
    }
    if (2 + base.len > cap) {
        return 0;
    }
    /* base.len already includes the 16-bit UUID */
    buf[0] = 1 + base.len;
    buf[1] = EXAMPLE_AD_TYPE_SERVICE_DATA16;
    memcpy(buf + 2, base.data, base.len);
    return 2 + base.len;
}

esp_err_t broadcast_start(broadcast_tick_cb_t tick, void *arg)
{
    static uint8_t ext[64];
    static uint8_t per[160];

    s_tick = tick;
    s_tick_arg = arg;
    s_broadcast_id = esp_random() & 0xFFFFFF;

    esp_err_t err = source_create();
    if (err) {
        return err;
    }
    err = esp_ble_audio_common_start(NULL);
    if (err) {
        ESP_LOGE(TAG, "audio start failed, err %d", err);
        return err;
    }

    uint8_t ext_len = build_ext_adv(ext, sizeof(ext));
    uint8_t per_len = build_per_adv(per, sizeof(per));
    if (ext_len == 0 || per_len == 0) {
        return ESP_FAIL;
    }
    if (adv_start(ext, ext_len, per, per_len)) {
        return ESP_FAIL;
    }

    esp_ble_audio_bap_broadcast_adv_info_t info = {
        .adv_handle = ADV_HANDLE,
    };
    err = esp_ble_audio_bap_broadcast_adv_add(&info);
    if (err) {
        ESP_LOGE(TAG, "adv add failed, err %d", err);
        return err;
    }
    err = esp_ble_audio_bap_broadcast_source_start(s_source, ADV_HANDLE);
    if (err) {
        ESP_LOGE(TAG, "source start failed, err %d", err);
        return err;
    }
    ESP_LOGI(TAG, "started name='%s' id=0x%06lx preset=%s bis=%d enc=%d", CONFIG_AURACAST_BROADCAST_NAME,
             (unsigned long)s_broadcast_id, PRESET_NAME, STREAM_COUNT, encrypted());
    return ESP_OK;
}

bool broadcast_streaming(void)
{
    return atomic_load(&s_streaming);
}

uint32_t broadcast_pairs_wanted(void)
{
    if (!broadcast_streaming()) {
        return 0;
    }
    return tx_pairs_wanted(s_seq, (uint16_t)atomic_load(&s_last_done), BROADCAST_TX_DEPTH);
}

esp_err_t broadcast_send_pair(const uint8_t *sdu_l, const uint8_t *sdu_r, uint16_t len)
{
    if (!broadcast_streaming()) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t *sdu[STREAM_COUNT] = {sdu_l, sdu_r};
    esp_err_t ret = ESP_OK;
    for (int i = 0; i < STREAM_COUNT; i++) {
        esp_err_t err = esp_ble_audio_bap_stream_send(&s_streams[i].stream, sdu[i], len, s_seq);
        if (err) {
            atomic_fetch_add(&s_send_err, 1);
            ret = err;
        }
    }
    s_seq++;
    return ret;
}

void broadcast_reset_in_flight(void)
{
    /* No completion for a while: assume everything was transmitted. Late callbacks re-sync it. */
    atomic_store(&s_last_done, (uint16_t)(s_seq - 1u));
    atomic_fetch_add(&s_wd_refills, 1);
}

uint16_t broadcast_sdu_len(void)
{
    return s_preset.qos.sdu;
}

void broadcast_get_stats(broadcast_stats_t *out, bool reset_window)
{
    out->sent = atomic_load(&s_sent);
    out->send_err = atomic_load(&s_send_err);
    out->wd_refills = atomic_load(&s_wd_refills);
    portENTER_CRITICAL(&s_stats_lock);
    out->tick_us_min = s_tick_n ? s_tick_min : 0;
    out->tick_us_max = s_tick_max;
    out->tick_us_mean = s_tick_n ? (double)s_tick_sum / s_tick_n : 0.0;
    int64_t span = s_last_tick_us - s_first_tick_us;
    out->bt_ppm = (s_ticks_total > 1 && span > 0)
                  ? ((double)(s_ticks_total - 1) * s_preset.qos.interval / (double)span - 1.0) * 1e6 : 0.0;
    if (reset_window) {
        s_tick_min = UINT32_MAX;
        s_tick_max = 0;
        s_tick_sum = 0;
        s_tick_n = 0;
    }
    portEXIT_CRITICAL(&s_stats_lock);
}

void broadcast_set_tx_power(void)
{
    static const int8_t dbm[] = {-24, -21, -18, -15, -12, -9, -6, -3, 0, 3, 6, 9, 12, 15, 16, 17, 18, 19, 20};
    int level = 0;
    for (int i = 0; i < (int)(sizeof(dbm) / sizeof(dbm[0])); i++) {
        if (dbm[i] <= CONFIG_AURACAST_TX_POWER_DBM) {
            level = i; /* ESP_PWR_LVL_N24 = 0 ... ESP_PWR_LVL_P20 = 18 */
        }
    }
    esp_err_t err = esp_ble_tx_power_set_enhanced(ESP_BLE_ENHANCED_PWR_TYPE_DEFAULT, 0, (esp_power_level_t)level);
    ESP_LOGI(TAG, "default TX power %+d dBm (level %d): %s", dbm[level], level, esp_err_to_name(err));
}

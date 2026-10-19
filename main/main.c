/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * AES67 -> Auracast bridge: I2S slave input (FPGA) re-broadcast as LE Audio (LC3, 1 or 2 BIS).
 * After boot it waits for its stream configuration from the I2C master (ctrl_i2c.c).
 */
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "esp_ble_audio_common_api.h"

#include "ble_audio_example_init.h"

#include "adv.h"
#include "broadcast.h"
#include "ctrl_i2c.h"
#include "pipeline.h"

static const char *TAG = "main";

static esp_err_t start_broadcast(const ctrl_config_t *cfg)
{
    broadcast_set_tx_power(cfg->tx_power_dbm);
    esp_err_t err = broadcast_configure(cfg);
    if (err == ESP_OK) {
        err = pipeline_start(cfg);
    }
    if (err == ESP_OK) {
        err = broadcast_start(pipeline_tick, NULL);
    }
    return err;
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "auracast-s31 boot, IDF %s", esp_get_idf_version());

    ESP_ERROR_CHECK(bluetooth_init());
    if (adv_host_init() != 0) {
        ESP_LOGE(TAG, "GAP init failed");
        return;
    }
    ESP_ERROR_CHECK(esp_ble_audio_common_init(NULL));
    ESP_ERROR_CHECK(ctrl_i2c_start());

    ctrl_config_t cfg;
    ctrl_i2c_wait_start(&cfg);
    err = start_broadcast(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "broadcast start failed: %s", esp_err_to_name(err));
        ctrl_i2c_set_state(AURA_STATE_FAULT, AURA_ERR_START_FAILED);
    }
}

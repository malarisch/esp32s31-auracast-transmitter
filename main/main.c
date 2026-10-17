/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * AES67 -> Auracast bridge: I2S slave input (FPGA) re-broadcast as LE Audio (LC3, 2 BIS).
 */
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "esp_ble_audio_common_api.h"

#include "ble_audio_example_init.h"

#include "adv.h"
#include "broadcast.h"
#include "pipeline.h"

static const char *TAG = "main";

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
    broadcast_set_tx_power();
    ESP_ERROR_CHECK(pipeline_start());
    ESP_ERROR_CHECK(broadcast_start(pipeline_tick, NULL));
}

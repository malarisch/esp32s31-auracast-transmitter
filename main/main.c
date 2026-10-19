/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * AES67 -> Auracast bridge: I2S slave input (FPGA) re-broadcast as LE Audio (LC3, 1 or 2 BIS).
 */
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "esp_ble_audio_common_api.h"

#include "ble_audio_example_init.h"

#include "adv.h"
#include "broadcast.h"
#include "ctrl_regs.h"
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

    static ctrl_regs_t regs;
    static const uint8_t fw[3] = {0, 0, 0};
    ctrl_config_t cfg;
    uint8_t bad;
    ctrl_regs_init(&regs, fw, 0);
    ESP_ERROR_CHECK(ctrl_regs_get_config(&regs, &cfg, &bad) == AURA_ERR_NONE ? ESP_OK : ESP_ERR_INVALID_ARG);

    broadcast_set_tx_power(cfg.tx_power_dbm);
    ESP_ERROR_CHECK(broadcast_configure(&cfg));
    ESP_ERROR_CHECK(pipeline_start(&cfg));
    ESP_ERROR_CHECK(broadcast_start(pipeline_tick, NULL));
}

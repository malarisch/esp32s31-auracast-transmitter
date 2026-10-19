/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#define ADV_HANDLE          0
#define ADV_SID             0
#define ADV_INTERVAL_MS     200
#define PER_ADV_INTERVAL_MS 100

/* Registers the GAP callback; call once after bluetooth_init() */
int adv_host_init(void);

/* Non-connectable extended advertising (ext_data) plus periodic advertising (per_data, the BASE) */
int adv_start(const uint8_t *ext_data, uint8_t ext_len, const uint8_t *per_data, uint8_t per_len,
              int8_t tx_power_dbm);

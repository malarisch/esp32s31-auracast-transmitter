/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "ctrl_regs.h"

/* SDUs kept queued in the controller per BIS */
#define BROADCAST_TX_DEPTH 2

/* Runs in the ISO task: only notify, never block */
typedef void (*broadcast_tick_cb_t)(void *arg);

typedef struct {
    uint32_t sent;            /* SDU sets confirmed by the controller (stream 0) */
    uint32_t send_err;
    uint32_t wd_refills;
    uint32_t tick_us_min;     /* window since the last reset */
    uint32_t tick_us_max;
    double tick_us_mean;
    double bt_ppm;            /* SDU tick rate vs. esp_timer since streaming start (cumulative) */
} broadcast_stats_t;

/* Takes a validated configuration (preset, channels, ...). Call before pipeline_start(). */
esp_err_t broadcast_configure(const ctrl_config_t *cfg);

/* Creates the broadcast source and starts advertising + BIG. Bluetooth must be initialized. */
esp_err_t broadcast_start(broadcast_tick_cb_t tick, void *arg);

/* All BIS are in STREAMING state */
bool broadcast_streaming(void);

/* Number of BIS: 2 for stereo, 1 for mono */
uint8_t broadcast_stream_count(void);

/* LC3 sample rate of the BIS: 16000, 24000 or 48000 */
uint32_t broadcast_sample_rate(void);

/* Broadcast_ID in use (24 bit) */
uint32_t broadcast_id(void);

/* SDU sets (one SDU per BIS) the controller queue can take now: BROADCAST_TX_DEPTH - in flight (stream 0) */
uint32_t broadcast_frames_wanted(void);

/* Sends broadcast_stream_count() SDUs, one per BIS, with the same sequence number */
esp_err_t broadcast_send_frame(const uint8_t *const sdu[], uint16_t len);

/* Watchdog recovery: assume the controller queue is empty */
void broadcast_reset_in_flight(void);

/* SDU length of the active preset (40/60/100/120/155 bytes) */
uint16_t broadcast_sdu_len(void);

void broadcast_get_stats(broadcast_stats_t *out, bool reset_window);

/* Applies tx_power_dbm (rounded down to a supported level) as default BLE TX power */
void broadcast_set_tx_power(int8_t tx_power_dbm);

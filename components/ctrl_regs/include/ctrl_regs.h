/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Register file of the I2C control interface (protocol: auracast_i2c_regs.h). Pure C, no locking:
 * the caller serialises access.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "auracast_i2c_regs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CTRL_CMD_NONE,
    CTRL_CMD_START,
    CTRL_CMD_RESET,
} ctrl_cmd_t;

/* Validated stream configuration */
typedef struct {
    uint8_t preset;             /* AURA_PRESET_* */
    uint8_t channels;           /* AURA_CH_* */
    uint16_t context;
    bool immediate;
    int8_t tx_power_dbm;
    uint16_t pd_us;
    uint16_t fifo_target;
    uint32_t broadcast_id;      /* AURA_BROADCAST_ID_RANDOM or 24 bit */
    char name[AURA_NAME_SIZE + 1];
    uint8_t code[AURA_CODE_SIZE];
    uint8_t code_len;           /* 0 = unencrypted */
    uint8_t hq_preset;          /* AURA_PRESET_48_* or AURA_PRESET_NONE (no HQ subgroup) */
    uint8_t hq_channels;        /* AURA_CH_* of the HQ subgroup (valid if hq_preset != NONE) */
    uint16_t hq_context;
} ctrl_config_t;

typedef struct {
    uint8_t pipe_state;         /* AURA_PIPE_* */
    uint8_t flags;              /* AURA_STATUS_* */
    uint16_t fifo_fill;
    int32_t drift_cppm;
    int32_t in_cppm;
    uint32_t i2s_bits;
    uint32_t underrun;
    uint32_t overflow;
    uint32_t lost;
    uint32_t send_err;
    uint32_t sent;
    uint32_t broadcast_id;
    uint32_t uptime_ms;
} ctrl_status_t;

typedef struct {
    uint8_t mem[256];
    uint8_t ptr;                /* register pointer, auto-incremented */
} ctrl_regs_t;

/* Identification, power-on configuration values, state WAIT_CONFIG */
void ctrl_regs_init(ctrl_regs_t *r, const uint8_t fw_version[3], uint8_t reset_reason);

/*
 * One master write transaction: buf[0] sets the register pointer, the rest is written from there.
 * Returns CTRL_CMD_START for a START accepted in WAIT_CONFIG/CONFIG_ERROR (the caller validates
 * with ctrl_regs_get_config() and sets the state), CTRL_CMD_RESET for RESET, otherwise NONE.
 */
ctrl_cmd_t ctrl_regs_write(ctrl_regs_t *r, const uint8_t *buf, size_t len);

/* One master read: len bytes from the register pointer (auto-increment). Returns len. */
size_t ctrl_regs_read(ctrl_regs_t *r, uint8_t *out, size_t len);

/* Validates the configuration registers. On error *bad_reg is the first address of the bad field. */
uint8_t ctrl_regs_get_config(const ctrl_regs_t *r, ctrl_config_t *out, uint8_t *bad_reg);

void ctrl_regs_set_state(ctrl_regs_t *r, uint8_t state, uint8_t err, uint8_t err_reg);
uint8_t ctrl_regs_state(const ctrl_regs_t *r);

void ctrl_regs_set_status(ctrl_regs_t *r, const ctrl_status_t *s);

/* Preset properties: LC3 sample rate in Hz and octets per frame (= SDU length) */
uint32_t ctrl_preset_rate(uint8_t preset);
uint16_t ctrl_preset_sdu(uint8_t preset);

/* High Quality preset (48 kHz) */
bool ctrl_preset_is_hq(uint8_t preset);

/* Number of BIS for a channel mode */
uint8_t ctrl_channels_bis(uint8_t channels);

/* Total number of BIS of a validated configuration: both subgroups (1..4) */
uint8_t ctrl_config_bis_total(const ctrl_config_t *c);

#ifdef __cplusplus
}
#endif

/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include "ctrl_regs.h"

static const struct {
    uint32_t rate;
    uint16_t sdu;
} s_presets[AURA_PRESET_COUNT] = {
    [AURA_PRESET_16_2_1] = {16000, 40},
    [AURA_PRESET_24_2_1] = {24000, 60},
    [AURA_PRESET_48_2_1] = {48000, 100},
    [AURA_PRESET_48_4_1] = {48000, 120},
    [AURA_PRESET_48_6_1] = {48000, 155},
};

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)get16(p) | (uint32_t)get16(p + 2) << 16;
}

static bool is_config_reg(uint8_t a)
{
    return (a >= AURA_REG_PRESET && a < AURA_REG_BROADCAST_ID + 4) ||
           (a >= AURA_REG_NAME && a < AURA_REG_CODE + AURA_CODE_SIZE) ||
           (a >= AURA_REG_HQ_PRESET && a < AURA_REG_HQ_CONTEXT + 2);
}

static bool config_unlocked(const ctrl_regs_t *r)
{
    uint8_t st = r->mem[AURA_REG_STATE];
    return st == AURA_STATE_WAIT_CONFIG || st == AURA_STATE_CONFIG_ERROR;
}

void ctrl_regs_init(ctrl_regs_t *r, const uint8_t fw_version[3], uint8_t reset_reason)
{
    memset(r, 0, sizeof(*r));
    memcpy(&r->mem[AURA_REG_MAGIC], "AURA", 4);
    r->mem[AURA_REG_PROTO_VERSION] = AURA_PROTO_VERSION;
    memcpy(&r->mem[AURA_REG_FW_VERSION], fw_version, 3);
    r->mem[AURA_REG_STATE] = AURA_STATE_WAIT_CONFIG;
    r->mem[AURA_REG_RESET_REASON] = reset_reason;

    r->mem[AURA_REG_PRESET] = AURA_DEFAULT_PRESET;
    r->mem[AURA_REG_CHANNELS] = AURA_DEFAULT_CHANNELS;
    put16(&r->mem[AURA_REG_CONTEXT], AURA_DEFAULT_CONTEXT);
    r->mem[AURA_REG_FLAGS] = AURA_DEFAULT_FLAGS;
    r->mem[AURA_REG_TX_POWER] = (uint8_t)(int8_t)AURA_DEFAULT_TX_POWER;
    put16(&r->mem[AURA_REG_PD_US], AURA_DEFAULT_PD_US);
    put16(&r->mem[AURA_REG_FIFO_TARGET], AURA_DEFAULT_FIFO_TARGET);
    put32(&r->mem[AURA_REG_BROADCAST_ID], AURA_DEFAULT_BROADCAST_ID);
    memcpy(&r->mem[AURA_REG_NAME], AURA_DEFAULT_NAME, sizeof(AURA_DEFAULT_NAME) - 1);
    r->mem[AURA_REG_HQ_PRESET] = AURA_DEFAULT_HQ_PRESET;
    r->mem[AURA_REG_HQ_CHANNELS] = AURA_DEFAULT_HQ_CHANNELS;
    put16(&r->mem[AURA_REG_HQ_CONTEXT], AURA_DEFAULT_HQ_CONTEXT);
}

ctrl_cmd_t ctrl_regs_write(ctrl_regs_t *r, const uint8_t *buf, size_t len)
{
    if (len == 0) {
        return CTRL_CMD_NONE;
    }
    r->ptr = buf[0];
    ctrl_cmd_t cmd = CTRL_CMD_NONE;
    uint8_t err = AURA_ERR_NONE, err_reg = 0;

    for (size_t i = 1; i < len; i++) {
        const uint8_t a = r->ptr++;
        const uint8_t v = buf[i];
        uint8_t e = AURA_ERR_NONE;
        if (a == AURA_REG_COMMAND) {
            if (v == AURA_CMD_RESET) {
                cmd = CTRL_CMD_RESET;
            } else if (v != AURA_CMD_START) {
                e = AURA_ERR_BAD_COMMAND;
            } else if (!config_unlocked(r)) {
                e = AURA_ERR_LOCKED;
            } else if (cmd != CTRL_CMD_RESET) {
                cmd = CTRL_CMD_START;
            }
        } else if (!is_config_reg(a)) {
            e = AURA_ERR_READ_ONLY;
        } else if (!config_unlocked(r)) {
            e = AURA_ERR_LOCKED;
        } else {
            r->mem[a] = v;
        }
        if (e != AURA_ERR_NONE && err == AURA_ERR_NONE) {
            err = e;
            err_reg = a;
        }
    }
    if (err != AURA_ERR_NONE) {
        r->mem[AURA_REG_ERROR] = err;
        r->mem[AURA_REG_ERROR_REG] = err_reg;
    }
    return cmd;
}

size_t ctrl_regs_read(ctrl_regs_t *r, uint8_t *out, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        out[i] = r->mem[r->ptr++];
    }
    return len;
}

/* Length of a zero-padded string field: bytes up to the first 0x00 */
static size_t field_len(const uint8_t *p, size_t size)
{
    const uint8_t *z = memchr(p, 0, size);
    return z ? (size_t)(z - p) : size;
}

uint8_t ctrl_regs_get_config(const ctrl_regs_t *r, ctrl_config_t *out, uint8_t *bad_reg)
{
    const uint8_t *m = r->mem;
    memset(out, 0, sizeof(*out));
    out->preset = m[AURA_REG_PRESET];
    out->channels = m[AURA_REG_CHANNELS];
    out->context = get16(&m[AURA_REG_CONTEXT]);
    out->immediate = (m[AURA_REG_FLAGS] & AURA_FLAG_IMMEDIATE) != 0;
    out->tx_power_dbm = (int8_t)m[AURA_REG_TX_POWER];
    out->pd_us = get16(&m[AURA_REG_PD_US]);
    out->fifo_target = get16(&m[AURA_REG_FIFO_TARGET]);
    out->broadcast_id = get32(&m[AURA_REG_BROADCAST_ID]);
    size_t name_len = field_len(&m[AURA_REG_NAME], AURA_NAME_SIZE);
    memcpy(out->name, &m[AURA_REG_NAME], name_len);
    out->code_len = (uint8_t)field_len(&m[AURA_REG_CODE], AURA_CODE_SIZE);
    memcpy(out->code, &m[AURA_REG_CODE], out->code_len);
    out->hq_preset = m[AURA_REG_HQ_PRESET];
    out->hq_channels = m[AURA_REG_HQ_CHANNELS];
    out->hq_context = get16(&m[AURA_REG_HQ_CONTEXT]);
    const bool hq = out->hq_preset != AURA_PRESET_NONE;

    uint8_t bad;
    if (out->preset >= AURA_PRESET_COUNT) {
        bad = AURA_REG_PRESET;
    } else if (out->channels >= AURA_CH_COUNT) {
        bad = AURA_REG_CHANNELS;
    } else if (out->context == 0 || (out->context & ~AURA_CONTEXT_MASK)) {
        bad = AURA_REG_CONTEXT;
    } else if (m[AURA_REG_FLAGS] & ~AURA_FLAG_IMMEDIATE) {
        bad = AURA_REG_FLAGS;
    } else if (out->tx_power_dbm < AURA_TX_POWER_MIN || out->tx_power_dbm > AURA_TX_POWER_MAX) {
        bad = AURA_REG_TX_POWER;
    } else if (out->pd_us < AURA_PD_US_MIN || out->pd_us > AURA_PD_US_MAX) {
        bad = AURA_REG_PD_US;
    } else if (out->fifo_target < AURA_FIFO_TARGET_MIN || out->fifo_target > AURA_FIFO_TARGET_MAX) {
        bad = AURA_REG_FIFO_TARGET;
    } else if (out->broadcast_id != AURA_BROADCAST_ID_RANDOM && out->broadcast_id > 0xFFFFFFu) {
        bad = AURA_REG_BROADCAST_ID;
    } else if (name_len < AURA_NAME_MIN_LEN) {
        bad = AURA_REG_NAME;
    } else if (hq && (!ctrl_preset_is_hq(out->hq_preset) || ctrl_preset_is_hq(out->preset))) {
        /* the HQ subgroup must be 48 kHz, and the first subgroup must then be Standard Quality */
        bad = AURA_REG_HQ_PRESET;
    } else if (hq && out->hq_channels >= AURA_CH_COUNT) {
        bad = AURA_REG_HQ_CHANNELS;
    } else if (hq && (out->hq_context == 0 || (out->hq_context & ~AURA_CONTEXT_MASK))) {
        bad = AURA_REG_HQ_CONTEXT;
    } else {
        return AURA_ERR_NONE;
    }
    *bad_reg = bad;
    return AURA_ERR_BAD_VALUE;
}

void ctrl_regs_set_state(ctrl_regs_t *r, uint8_t state, uint8_t err, uint8_t err_reg)
{
    r->mem[AURA_REG_STATE] = state;
    r->mem[AURA_REG_ERROR] = err;
    r->mem[AURA_REG_ERROR_REG] = err_reg;
}

uint8_t ctrl_regs_state(const ctrl_regs_t *r)
{
    return r->mem[AURA_REG_STATE];
}

void ctrl_regs_set_status(ctrl_regs_t *r, const ctrl_status_t *s)
{
    uint8_t *m = r->mem;
    m[AURA_REG_PIPE_STATE] = s->pipe_state;
    m[AURA_REG_STATUS_FLAGS] = s->flags;
    put16(&m[AURA_REG_FIFO_FILL], s->fifo_fill);
    put32(&m[AURA_REG_DRIFT_CPPM], (uint32_t)s->drift_cppm);
    put32(&m[AURA_REG_IN_CPPM], (uint32_t)s->in_cppm);
    put32(&m[AURA_REG_I2S_BITS], s->i2s_bits);
    put32(&m[AURA_REG_UNDERRUN], s->underrun);
    put32(&m[AURA_REG_OVERFLOW], s->overflow);
    put32(&m[AURA_REG_LOST], s->lost);
    put32(&m[AURA_REG_SEND_ERR], s->send_err);
    put32(&m[AURA_REG_SENT], s->sent);
    put32(&m[AURA_REG_ACTIVE_ID], s->broadcast_id);
    put32(&m[AURA_REG_UPTIME_MS], s->uptime_ms);
}

uint32_t ctrl_preset_rate(uint8_t preset)
{
    return preset < AURA_PRESET_COUNT ? s_presets[preset].rate : 0;
}

uint16_t ctrl_preset_sdu(uint8_t preset)
{
    return preset < AURA_PRESET_COUNT ? s_presets[preset].sdu : 0;
}

bool ctrl_preset_is_hq(uint8_t preset)
{
    return ctrl_preset_rate(preset) == 48000;
}

uint8_t ctrl_channels_bis(uint8_t channels)
{
    return channels == AURA_CH_STEREO ? 2 : 1;
}

uint8_t ctrl_config_bis_total(const ctrl_config_t *c)
{
    uint8_t n = ctrl_channels_bis(c->channels);
    if (c->hq_preset != AURA_PRESET_NONE) {
        n += ctrl_channels_bis(c->hq_channels);
    }
    return n;
}

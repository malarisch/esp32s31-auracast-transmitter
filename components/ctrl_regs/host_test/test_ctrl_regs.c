/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>

#include "ctrl_regs.h"

static int g_test_failures;

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);         \
            g_test_failures++;                                                               \
        }                                                                                    \
    } while (0)

#define RUN_TEST(fn)                                                                         \
    do {                                                                                     \
        int before_ = g_test_failures;                                                       \
        fn();                                                                                \
        printf("%s %s\n", g_test_failures == before_ ? "PASS" : "FAIL", #fn);                \
    } while (0)

static const uint8_t FW[3] = {1, 2, 3};

static void fresh(ctrl_regs_t *r)
{
    ctrl_regs_init(r, FW, 4);
}

/* Master write [reg][data...] */
static ctrl_cmd_t wr(ctrl_regs_t *r, uint8_t reg, const void *data, size_t n)
{
    uint8_t buf[260];
    buf[0] = reg;
    memcpy(buf + 1, data, n);
    return ctrl_regs_write(r, buf, n + 1);
}

static ctrl_cmd_t wr8(ctrl_regs_t *r, uint8_t reg, uint8_t v)
{
    return wr(r, reg, &v, 1);
}

static void wr16(ctrl_regs_t *r, uint8_t reg, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    wr(r, reg, b, 2);
}

static void wr32(ctrl_regs_t *r, uint8_t reg, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    wr(r, reg, b, 4);
}

/* Master write_read: [reg], repeated start, read n */
static void rd(ctrl_regs_t *r, uint8_t reg, void *out, size_t n)
{
    ctrl_regs_write(r, &reg, 1);
    ctrl_regs_read(r, out, n);
}

static uint8_t rd8(ctrl_regs_t *r, uint8_t reg)
{
    uint8_t v;
    rd(r, reg, &v, 1);
    return v;
}

static uint16_t rd16(ctrl_regs_t *r, uint8_t reg)
{
    uint8_t b[2];
    rd(r, reg, b, 2);
    return (uint16_t)(b[0] | b[1] << 8);
}

static uint32_t rd32(ctrl_regs_t *r, uint8_t reg)
{
    uint8_t b[4];
    rd(r, reg, b, 4);
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static void set_name(ctrl_regs_t *r, const char *name)
{
    uint8_t buf[AURA_NAME_SIZE] = {0};
    memcpy(buf, name, strlen(name) > AURA_NAME_SIZE ? AURA_NAME_SIZE : strlen(name));
    wr(r, AURA_REG_NAME, buf, sizeof(buf));
}

static void test_init_magic_defaults(void)
{
    ctrl_regs_t r;
    fresh(&r);
    uint8_t magic[4];
    rd(&r, AURA_REG_MAGIC, magic, 4);
    CHECK(memcmp(magic, "AURA", 4) == 0);
    CHECK(rd8(&r, AURA_REG_PROTO_VERSION) == AURA_PROTO_VERSION);
    uint8_t fw[3];
    rd(&r, AURA_REG_FW_VERSION, fw, 3);
    CHECK(memcmp(fw, FW, 3) == 0);
    CHECK(rd8(&r, AURA_REG_STATE) == AURA_STATE_WAIT_CONFIG);
    CHECK(rd8(&r, AURA_REG_ERROR) == AURA_ERR_NONE);
    CHECK(rd8(&r, AURA_REG_RESET_REASON) == 4);
    CHECK(rd8(&r, AURA_REG_PRESET) == AURA_PRESET_24_2_1);
    CHECK(rd8(&r, AURA_REG_CHANNELS) == AURA_CH_MONO_MIX);
    CHECK(rd16(&r, AURA_REG_CONTEXT) == 0x0040);
    CHECK(rd8(&r, AURA_REG_FLAGS) == AURA_FLAG_IMMEDIATE);
    CHECK((int8_t)rd8(&r, AURA_REG_TX_POWER) == 9);
    CHECK(rd16(&r, AURA_REG_PD_US) == 20000);
    CHECK(rd16(&r, AURA_REG_FIFO_TARGET) == 840);
    CHECK(rd32(&r, AURA_REG_BROADCAST_ID) == AURA_BROADCAST_ID_RANDOM);

    ctrl_config_t c;
    uint8_t bad = 0xEE;
    CHECK(ctrl_regs_get_config(&r, &c, &bad) == AURA_ERR_NONE);
    CHECK(c.preset == AURA_PRESET_24_2_1 && c.channels == AURA_CH_MONO_MIX && c.context == 0x0040);
    CHECK(c.immediate && c.tx_power_dbm == 9 && c.pd_us == 20000 && c.fifo_target == 840);
    CHECK(c.broadcast_id == AURA_BROADCAST_ID_RANDOM);
    CHECK(strcmp(c.name, "AES67 Auracast") == 0);
    CHECK(c.code_len == 0);
}

static void test_burst_write_read_le(void)
{
    ctrl_regs_t r;
    fresh(&r);
    /* PD_US and FIFO_TARGET in one burst */
    const uint8_t b[4] = {0x40, 0x9C, 0xE8, 0x03};   /* 40000, 1000 */
    CHECK(wr(&r, AURA_REG_PD_US, b, 4) == CTRL_CMD_NONE);
    CHECK(rd16(&r, AURA_REG_PD_US) == 40000);
    CHECK(rd16(&r, AURA_REG_FIFO_TARGET) == 1000);
    uint8_t back[4];
    rd(&r, AURA_REG_PD_US, back, 4);
    CHECK(memcmp(back, b, 4) == 0);
    CHECK(rd8(&r, AURA_REG_ERROR) == AURA_ERR_NONE);
    /* Consecutive reads continue at the pointer */
    uint8_t m[2];
    uint8_t reg = AURA_REG_MAGIC;
    ctrl_regs_write(&r, &reg, 1);
    ctrl_regs_read(&r, m, 2);
    CHECK(m[0] == 'A' && m[1] == 'U');
    ctrl_regs_read(&r, m, 2);
    CHECK(m[0] == 'R' && m[1] == 'A');
}

static void test_write_spans_readonly(void)
{
    ctrl_regs_t r;
    fresh(&r);
    /* 0x1E/0x1F are reserved: a burst from BROADCAST_ID over them into NAME */
    uint8_t b[8] = {1, 2, 3, 0, 0xAA, 0xBB, 'X', 'Y'};
    wr(&r, AURA_REG_BROADCAST_ID, b, sizeof(b));
    CHECK(rd32(&r, AURA_REG_BROADCAST_ID) == 0x030201);
    CHECK(rd8(&r, 0x1E) == 0 && rd8(&r, 0x1F) == 0);
    CHECK(rd8(&r, AURA_REG_NAME) == 'X' && rd8(&r, AURA_REG_NAME + 1) == 'Y');
    CHECK(rd8(&r, AURA_REG_ERROR) == AURA_ERR_READ_ONLY);
    CHECK(rd8(&r, AURA_REG_ERROR_REG) == 0x1E);
    /* Identification cannot be overwritten */
    wr8(&r, AURA_REG_MAGIC, 'Z');
    CHECK(rd8(&r, AURA_REG_MAGIC) == 'A');
    wr8(&r, AURA_REG_STATE, AURA_STATE_STREAMING);
    CHECK(rd8(&r, AURA_REG_STATE) == AURA_STATE_WAIT_CONFIG);
    wr8(&r, AURA_REG_UNDERRUN, 7);
    CHECK(rd8(&r, AURA_REG_UNDERRUN) == 0);
    CHECK(rd8(&r, AURA_REG_ERROR_REG) == AURA_REG_UNDERRUN);
}

static void test_command_start_reset_bad(void)
{
    ctrl_regs_t r;
    fresh(&r);
    CHECK(wr8(&r, AURA_REG_COMMAND, 0x11) == CTRL_CMD_NONE);
    CHECK(rd8(&r, AURA_REG_ERROR) == AURA_ERR_BAD_COMMAND);
    CHECK(rd8(&r, AURA_REG_ERROR_REG) == AURA_REG_COMMAND);
    CHECK(rd8(&r, AURA_REG_COMMAND) == 0);
    CHECK(wr8(&r, AURA_REG_COMMAND, AURA_CMD_START) == CTRL_CMD_START);
    CHECK(rd8(&r, AURA_REG_COMMAND) == 0);
    CHECK(wr8(&r, AURA_REG_COMMAND, AURA_CMD_RESET) == CTRL_CMD_RESET);
    /* A command inside a longer burst counts too */
    uint8_t b[2] = {AURA_CMD_RESET, 0};
    CHECK(wr(&r, AURA_REG_COMMAND, b, 2) == CTRL_CMD_RESET);
}

static void test_locked_after_start(void)
{
    ctrl_regs_t r;
    fresh(&r);
    ctrl_regs_set_state(&r, AURA_STATE_STARTING, AURA_ERR_NONE, 0);
    wr8(&r, AURA_REG_PRESET, AURA_PRESET_48_4_1);
    CHECK(rd8(&r, AURA_REG_PRESET) == AURA_PRESET_24_2_1);
    CHECK(rd8(&r, AURA_REG_ERROR) == AURA_ERR_LOCKED);
    CHECK(rd8(&r, AURA_REG_ERROR_REG) == AURA_REG_PRESET);
    set_name(&r, "Other");
    CHECK(rd8(&r, AURA_REG_NAME) == 'A');
    ctrl_regs_set_state(&r, AURA_STATE_STREAMING, AURA_ERR_NONE, 0);
    CHECK(wr8(&r, AURA_REG_COMMAND, AURA_CMD_START) == CTRL_CMD_NONE);
    CHECK(rd8(&r, AURA_REG_ERROR) == AURA_ERR_LOCKED);
    CHECK(rd8(&r, AURA_REG_ERROR_REG) == AURA_REG_COMMAND);
    /* RESET is always accepted */
    CHECK(wr8(&r, AURA_REG_COMMAND, AURA_CMD_RESET) == CTRL_CMD_RESET);
    ctrl_regs_set_state(&r, AURA_STATE_FAULT, AURA_ERR_START_FAILED, 0);
    CHECK(wr8(&r, AURA_REG_COMMAND, AURA_CMD_RESET) == CTRL_CMD_RESET);
}

static uint8_t start_err(ctrl_regs_t *r, uint8_t *bad)
{
    ctrl_config_t c;
    return ctrl_regs_get_config(r, &c, bad);
}

/* One field set just outside its range -> BAD_VALUE pointing at that field */
static void expect_bad(void (*mutate)(ctrl_regs_t *), uint8_t reg, const char *what)
{
    ctrl_regs_t r;
    fresh(&r);
    mutate(&r);
    uint8_t bad = 0;
    uint8_t err = start_err(&r, &bad);
    if (err != AURA_ERR_BAD_VALUE || bad != reg) {
        fprintf(stderr, "  %s: err %u reg 0x%02x (want 0x%02x)\n", what, err, bad, reg);
    }
    CHECK(err == AURA_ERR_BAD_VALUE);
    CHECK(bad == reg);
}

static void m_preset(ctrl_regs_t *r) { wr8(r, AURA_REG_PRESET, AURA_PRESET_COUNT); }
static void m_channels(ctrl_regs_t *r) { wr8(r, AURA_REG_CHANNELS, AURA_CH_COUNT); }
static void m_context(ctrl_regs_t *r) { wr16(r, AURA_REG_CONTEXT, 0); }
static void m_context_rfu(ctrl_regs_t *r) { wr16(r, AURA_REG_CONTEXT, 0x1040); }
static void m_flags(ctrl_regs_t *r) { wr8(r, AURA_REG_FLAGS, 0x02); }
static void m_tx_low(ctrl_regs_t *r) { wr8(r, AURA_REG_TX_POWER, (uint8_t)(int8_t)(AURA_TX_POWER_MIN - 1)); }
static void m_tx_high(ctrl_regs_t *r) { wr8(r, AURA_REG_TX_POWER, AURA_TX_POWER_MAX + 1); }
static void m_pd_low(ctrl_regs_t *r) { wr16(r, AURA_REG_PD_US, AURA_PD_US_MIN - 1); }
static void m_pd_high(ctrl_regs_t *r) { wr16(r, AURA_REG_PD_US, AURA_PD_US_MAX + 1); }
static void m_fifo_low(ctrl_regs_t *r) { wr16(r, AURA_REG_FIFO_TARGET, AURA_FIFO_TARGET_MIN - 1); }
static void m_fifo_high(ctrl_regs_t *r) { wr16(r, AURA_REG_FIFO_TARGET, AURA_FIFO_TARGET_MAX + 1); }
static void m_id(ctrl_regs_t *r) { wr32(r, AURA_REG_BROADCAST_ID, 0x01000000); }
static void m_name_short(ctrl_regs_t *r) { set_name(r, "abc"); }

static void test_validation(void)
{
    expect_bad(m_preset, AURA_REG_PRESET, "preset");
    expect_bad(m_channels, AURA_REG_CHANNELS, "channels");
    expect_bad(m_context, AURA_REG_CONTEXT, "context");
    expect_bad(m_context_rfu, AURA_REG_CONTEXT, "context reserved bits");
    expect_bad(m_flags, AURA_REG_FLAGS, "flags");
    expect_bad(m_tx_low, AURA_REG_TX_POWER, "tx low");
    expect_bad(m_tx_high, AURA_REG_TX_POWER, "tx high");
    expect_bad(m_pd_low, AURA_REG_PD_US, "pd low");
    expect_bad(m_pd_high, AURA_REG_PD_US, "pd high");
    expect_bad(m_fifo_low, AURA_REG_FIFO_TARGET, "fifo low");
    expect_bad(m_fifo_high, AURA_REG_FIFO_TARGET, "fifo high");
    expect_bad(m_id, AURA_REG_BROADCAST_ID, "broadcast id");
    expect_bad(m_name_short, AURA_REG_NAME, "name");

    /* Range limits themselves are valid */
    ctrl_regs_t r;
    fresh(&r);
    wr8(&r, AURA_REG_PRESET, AURA_PRESET_48_6_1);
    wr8(&r, AURA_REG_CHANNELS, AURA_CH_MONO_RIGHT);
    wr8(&r, AURA_REG_TX_POWER, (uint8_t)(int8_t)AURA_TX_POWER_MIN);
    wr16(&r, AURA_REG_PD_US, AURA_PD_US_MAX);
    wr16(&r, AURA_REG_FIFO_TARGET, AURA_FIFO_TARGET_MAX);
    wr32(&r, AURA_REG_BROADCAST_ID, 0xFFFFFF);
    wr8(&r, AURA_REG_FLAGS, 0);
    ctrl_config_t c;
    uint8_t bad;
    CHECK(ctrl_regs_get_config(&r, &c, &bad) == AURA_ERR_NONE);
    CHECK(c.preset == AURA_PRESET_48_6_1 && c.channels == AURA_CH_MONO_RIGHT && c.tx_power_dbm == -15);
    CHECK(c.pd_us == 40000 && c.fifo_target == 3000 && c.broadcast_id == 0xFFFFFF && !c.immediate);
}

static void test_name_bounds(void)
{
    ctrl_regs_t r;
    ctrl_config_t c;
    uint8_t bad;
    fresh(&r);
    set_name(&r, "Kirche");
    CHECK(ctrl_regs_get_config(&r, &c, &bad) == AURA_ERR_NONE && strcmp(c.name, "Kirche") == 0);
    set_name(&r, "Saal");
    CHECK(ctrl_regs_get_config(&r, &c, &bad) == AURA_ERR_NONE && strcmp(c.name, "Saal") == 0);
    /* 32 bytes, no terminating zero in the register */
    const char *n32 = "0123456789abcdef0123456789ABCDEF";
    set_name(&r, n32);
    CHECK(ctrl_regs_get_config(&r, &c, &bad) == AURA_ERR_NONE && strcmp(c.name, n32) == 0);
    /* All zero: empty name */
    set_name(&r, "");
    CHECK(ctrl_regs_get_config(&r, &c, &bad) == AURA_ERR_BAD_VALUE && bad == AURA_REG_NAME);
}

static void test_code_16(void)
{
    ctrl_regs_t r;
    ctrl_config_t c;
    uint8_t bad;
    fresh(&r);
    const char *code = "0123456789abcdef";
    wr(&r, AURA_REG_CODE, code, 16);
    CHECK(ctrl_regs_get_config(&r, &c, &bad) == AURA_ERR_NONE);
    CHECK(c.code_len == 16 && memcmp(c.code, code, 16) == 0);
    const uint8_t shorter[16] = {'p', 'w', '1', '2'};
    wr(&r, AURA_REG_CODE, shorter, 16);
    CHECK(ctrl_regs_get_config(&r, &c, &bad) == AURA_ERR_NONE);
    CHECK(c.code_len == 4 && memcmp(c.code, "pw12", 4) == 0);
}

static void test_restart_after_config_error(void)
{
    ctrl_regs_t r;
    ctrl_config_t c;
    uint8_t bad;
    fresh(&r);
    wr8(&r, AURA_REG_PRESET, 9);
    CHECK(wr8(&r, AURA_REG_COMMAND, AURA_CMD_START) == CTRL_CMD_START);
    CHECK(ctrl_regs_get_config(&r, &c, &bad) == AURA_ERR_BAD_VALUE);
    ctrl_regs_set_state(&r, AURA_STATE_CONFIG_ERROR, AURA_ERR_BAD_VALUE, bad);
    CHECK(rd8(&r, AURA_REG_STATE) == AURA_STATE_CONFIG_ERROR);
    CHECK(rd8(&r, AURA_REG_ERROR_REG) == AURA_REG_PRESET);
    /* Still writable, START accepted again */
    wr8(&r, AURA_REG_PRESET, AURA_PRESET_16_2_1);
    CHECK(rd8(&r, AURA_REG_PRESET) == AURA_PRESET_16_2_1);
    CHECK(wr8(&r, AURA_REG_COMMAND, AURA_CMD_START) == CTRL_CMD_START);
    CHECK(ctrl_regs_get_config(&r, &c, &bad) == AURA_ERR_NONE && c.preset == AURA_PRESET_16_2_1);
    ctrl_regs_set_state(&r, AURA_STATE_STARTING, AURA_ERR_NONE, 0);
    CHECK(ctrl_regs_state(&r) == AURA_STATE_STARTING);
    CHECK(rd8(&r, AURA_REG_ERROR) == AURA_ERR_NONE);
}

static void test_status_block_layout(void)
{
    ctrl_regs_t r;
    fresh(&r);
    const ctrl_status_t s = {
        .pipe_state = AURA_PIPE_RUN,
        .flags = AURA_STATUS_I2S_SIGNAL | AURA_STATUS_BIS_STREAMING,
        .fifo_fill = 841,
        .drift_cppm = -1234,
        .in_cppm = 567,
        .i2s_bits = 0x7FFFFF00,
        .underrun = 1,
        .overflow = 2,
        .lost = 3,
        .send_err = 4,
        .sent = 100000,
        .broadcast_id = 0xABCDEF,
        .uptime_ms = 987654,
    };
    ctrl_regs_set_status(&r, &s);
    CHECK(rd8(&r, AURA_REG_PIPE_STATE) == AURA_PIPE_RUN);
    CHECK(rd8(&r, AURA_REG_STATUS_FLAGS) == 3);
    CHECK(rd16(&r, AURA_REG_FIFO_FILL) == 841);
    CHECK((int32_t)rd32(&r, AURA_REG_DRIFT_CPPM) == -1234);
    CHECK((int32_t)rd32(&r, AURA_REG_IN_CPPM) == 567);
    CHECK(rd32(&r, AURA_REG_I2S_BITS) == 0x7FFFFF00);
    CHECK(rd32(&r, AURA_REG_UNDERRUN) == 1);
    CHECK(rd32(&r, AURA_REG_OVERFLOW) == 2);
    CHECK(rd32(&r, AURA_REG_LOST) == 3);
    CHECK(rd32(&r, AURA_REG_SEND_ERR) == 4);
    CHECK(rd32(&r, AURA_REG_SENT) == 100000);
    CHECK(rd32(&r, AURA_REG_ACTIVE_ID) == 0xABCDEF);
    CHECK(rd32(&r, AURA_REG_UPTIME_MS) == 987654);
    CHECK(rd8(&r, AURA_REG_STATUS_END) == 0);
}

static void test_pointer_wraps(void)
{
    ctrl_regs_t r;
    fresh(&r);
    uint8_t b[5];
    rd(&r, 0xFE, b, 5);
    CHECK(b[0] == 0 && b[1] == 0 && b[2] == 'A' && b[3] == 'U' && b[4] == 'R');
}

static void test_preset_tables(void)
{
    CHECK(ctrl_preset_rate(AURA_PRESET_16_2_1) == 16000 && ctrl_preset_sdu(AURA_PRESET_16_2_1) == 40);
    CHECK(ctrl_preset_rate(AURA_PRESET_24_2_1) == 24000 && ctrl_preset_sdu(AURA_PRESET_24_2_1) == 60);
    CHECK(ctrl_preset_rate(AURA_PRESET_48_2_1) == 48000 && ctrl_preset_sdu(AURA_PRESET_48_2_1) == 100);
    CHECK(ctrl_preset_rate(AURA_PRESET_48_4_1) == 48000 && ctrl_preset_sdu(AURA_PRESET_48_4_1) == 120);
    CHECK(ctrl_preset_rate(AURA_PRESET_48_6_1) == 48000 && ctrl_preset_sdu(AURA_PRESET_48_6_1) == 155);
    CHECK(ctrl_channels_bis(AURA_CH_STEREO) == 2);
    CHECK(ctrl_channels_bis(AURA_CH_MONO_MIX) == 1);
    CHECK(ctrl_channels_bis(AURA_CH_MONO_LEFT) == 1);
    CHECK(ctrl_channels_bis(AURA_CH_MONO_RIGHT) == 1);
}

int main(void)
{
    RUN_TEST(test_init_magic_defaults);
    RUN_TEST(test_burst_write_read_le);
    RUN_TEST(test_write_spans_readonly);
    RUN_TEST(test_command_start_reset_bad);
    RUN_TEST(test_locked_after_start);
    RUN_TEST(test_validation);
    RUN_TEST(test_name_bounds);
    RUN_TEST(test_code_16);
    RUN_TEST(test_restart_after_config_error);
    RUN_TEST(test_status_block_layout);
    RUN_TEST(test_pointer_wraps);
    RUN_TEST(test_preset_tables);
    return g_test_failures ? 1 : 0;
}

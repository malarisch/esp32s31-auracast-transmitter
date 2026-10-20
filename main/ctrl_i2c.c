/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * I2C slave front end of the control interface. The driver callbacks run in the ISR and only queue
 * events; the control task applies them to the register file:
 *   receive (master write, also the register address of a write_read) -> drop leftover TX data, ctrl_regs_write()
 *   request (master read; SCL is stretched until i2c_slave_write())   -> AURA_READ_MAX bytes from the pointer
 * A master read that stops early leaves bytes in the TX FIFO. They are dropped before the next fill (and at the
 * next write), but the slave hardware has by then pre-loaded one of them into its shift register, which the FIFO
 * reset does not reach (only the FSM reset of i2c_slave_reset_tx_fifo() does, and that is allowed only after a
 * completed read, for which there is no callback). That byte prefixes the next read. Hence the protocol: every
 * read sets the register pointer first (write_read) and takes exactly AURA_READ_MAX bytes, draining the FIFO.
 *
 * Why at most AURA_READ_MAX (31) bytes per read, and why the FIFO is reset right before every fill:
 * i2c_slave_write() puts only as many bytes as the hardware FIFO (32) has free straight into the FIFO; the rest
 * goes through the driver's ring buffer, from where the driver's ISR refills the FIFO whenever it drains. Bytes
 * in the ring buffer survive a plain TX FIFO reset and would be served to every later read, in front of the real
 * data (seen on the bench: a register-less read after a short read left 23 bytes in the FIFO, the next fill
 * overflowed into the ring buffer, and from then on every write_read returned stale register contents). With an
 * empty FIFO at fill time, 31 bytes always fit and the ring buffer is never used. i2c_slave_reset_tx_fifo()
 * would also drain the ring buffer, but it resets the SCL state machine as well, and the receive event of a
 * write_read arrives while the slave stretches SCL for the read: the IDF documents that reset for after a
 * completed read only.
 */
#include <string.h>

#include "driver/i2c_slave.h"
#include "esp_attr.h"
#include "hal/i2c_ll.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "broadcast.h"
#include "ctrl_i2c.h"
#include "pipeline.h"

#define RX_MAX           96      /* largest master write we keep: register + 64 config bytes + margin */
#define I2C_PORT         I2C_NUM_0
#define EVT_QUEUE_LEN    8
#define STATUS_PERIOD_MS 500
#define RESET_DELAY_MS   10

static const char *TAG = "ctrl_i2c";
static const uint8_t FW_VERSION[3] = {1, 0, 0};

typedef struct {
    uint8_t kind;               /* EVT_RX or EVT_REQUEST */
    uint8_t len;
    uint8_t data[RX_MAX];
} ctrl_evt_t;

enum { EVT_RX, EVT_REQUEST };

static i2c_slave_dev_handle_t s_dev;
static QueueHandle_t s_evt;
static SemaphoreHandle_t s_lock;          /* register file */
static SemaphoreHandle_t s_started;       /* given once on an accepted START */
static ctrl_regs_t s_regs;
static ctrl_config_t s_cfg;
static uint32_t s_rx_dropped;
static portMUX_TYPE s_fifo_lock = portMUX_INITIALIZER_UNLOCKED;

_Static_assert(AURA_READ_MAX < I2C_LL_FIFO_LEN, "a read must fit the hardware TX FIFO");

/* Drops bytes a shorter master read left in the TX FIFO (the ring buffer stays empty, see above) */
static void drop_tx_leftovers(void)
{
    portENTER_CRITICAL(&s_fifo_lock);   /* keeps the driver ISR (same core) out */
    i2c_ll_txfifo_rst(I2C_LL_GET_HW(I2C_PORT));
    portEXIT_CRITICAL(&s_fifo_lock);
}

static IRAM_ATTR bool on_receive(i2c_slave_dev_handle_t dev, const i2c_slave_rx_done_event_data_t *e, void *arg)
{
    ctrl_evt_t evt = {.kind = EVT_RX};
    if (e->length > RX_MAX) {
        s_rx_dropped++;
        return false;
    }
    evt.len = (uint8_t)e->length;
    memcpy(evt.data, e->buffer, e->length);
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_evt, &evt, &woken);
    return woken == pdTRUE;
}

static IRAM_ATTR bool on_request(i2c_slave_dev_handle_t dev, const i2c_slave_request_event_data_t *e, void *arg)
{
    ctrl_evt_t evt = {.kind = EVT_REQUEST};
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_evt, &evt, &woken);
    return woken == pdTRUE;
}

/* Register lock held */
static void handle_start(void)
{
    uint8_t bad = 0;
    uint8_t err = ctrl_regs_get_config(&s_regs, &s_cfg, &bad);
    if (err != AURA_ERR_NONE) {
        ctrl_regs_set_state(&s_regs, AURA_STATE_CONFIG_ERROR, err, bad);
        ESP_LOGW(TAG, "START rejected: error %u at register 0x%02x", err, bad);
        return;
    }
    ctrl_regs_set_state(&s_regs, AURA_STATE_STARTING, AURA_ERR_NONE, 0);
    ESP_LOGI(TAG, "START accepted");
    xSemaphoreGive(s_started);
}

static void handle_rx(const ctrl_evt_t *evt)
{
    /* A new transaction: bytes left from a shorter read must not reach the next one */
    drop_tx_leftovers();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ctrl_cmd_t cmd = ctrl_regs_write(&s_regs, evt->data, evt->len);
    if (cmd == CTRL_CMD_START) {
        handle_start();
    }
    xSemaphoreGive(s_lock);
    if (cmd == CTRL_CMD_RESET) {
        ESP_LOGW(TAG, "RESET requested");
        vTaskDelay(pdMS_TO_TICKS(RESET_DELAY_MS));   /* let the transaction finish */
        esp_restart();
    }
}

static void handle_request(void)
{
    uint8_t buf[AURA_READ_MAX];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ctrl_regs_read(&s_regs, buf, sizeof(buf));
    /* The master reads fewer bytes in general: the pointer follows what was queued, not what was clocked out */
    xSemaphoreGive(s_lock);
    /* Whatever a shorter (or register-less) read left behind goes first, so the FIFO is empty and the whole
     * answer fits into it; nothing must ever reach the driver's ring buffer (see the file comment). */
    drop_tx_leftovers();
    uint32_t written = 0;
    esp_err_t err = i2c_slave_write(s_dev, buf, sizeof(buf), &written, 50);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "slave write failed: %s", esp_err_to_name(err));
    } else if (written != sizeof(buf)) {
        ESP_LOGW(TAG, "slave write queued %u of %u bytes", (unsigned)written, (unsigned)sizeof(buf));
    }
}

static void ctrl_task(void *arg)
{
    ctrl_evt_t evt;
    for (;;) {
        if (xQueueReceive(s_evt, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (evt.kind == EVT_RX) {
            handle_rx(&evt);
        } else {
            handle_request();
        }
    }
}

static int32_t to_cppm(float ppm)
{
    return (int32_t)(ppm * 100.0f + (ppm >= 0.0f ? 0.5f : -0.5f));
}

static void status_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(STATUS_PERIOD_MS));
        ctrl_status_t st = {.uptime_ms = (uint32_t)(esp_timer_get_time() / 1000)};
        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint8_t state = ctrl_regs_state(&s_regs);
        xSemaphoreGive(s_lock);
        const bool running = state == AURA_STATE_STARTING || state == AURA_STATE_STREAMING;
        bool streaming = false;
        if (running) {
            pipeline_status_t p;
            broadcast_stats_t b;
            pipeline_get_status(&p);
            broadcast_get_stats(&b, false);
            streaming = broadcast_streaming();
            st.pipe_state = p.state;
            st.flags = (p.signal ? AURA_STATUS_I2S_SIGNAL : 0) | (streaming ? AURA_STATUS_BIS_STREAMING : 0);
            st.fifo_fill = p.fifo_fill > UINT16_MAX ? UINT16_MAX : (uint16_t)p.fifo_fill;
            st.drift_cppm = to_cppm(p.drift_ppm);
            st.in_cppm = to_cppm(p.in_ppm);
            st.i2s_bits = p.i2s_bits;
            st.underrun = p.underrun;
            st.overflow = p.overflow;
            st.lost = p.lost;
            st.send_err = b.send_err;
            st.sent = b.sent;
            st.broadcast_id = broadcast_id();
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        ctrl_regs_set_status(&s_regs, &st);
        if (ctrl_regs_state(&s_regs) == AURA_STATE_STARTING && streaming) {
            ctrl_regs_set_state(&s_regs, AURA_STATE_STREAMING, AURA_ERR_NONE, 0);
            ESP_LOGI(TAG, "state STREAMING");
        }
        xSemaphoreGive(s_lock);
    }
}

esp_err_t ctrl_i2c_start(void)
{
    ctrl_regs_init(&s_regs, FW_VERSION, (uint8_t)esp_reset_reason());
    s_evt = xQueueCreate(EVT_QUEUE_LEN, sizeof(ctrl_evt_t));
    s_lock = xSemaphoreCreateMutex();
    s_started = xSemaphoreCreateBinary();
    if (s_evt == NULL || s_lock == NULL || s_started == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const i2c_slave_config_t cfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = CONFIG_AURACAST_I2C_SDA_GPIO,
        .scl_io_num = CONFIG_AURACAST_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .send_buf_depth = 256,
        .receive_buf_depth = 256,
        .slave_addr = CONFIG_AURACAST_I2C_ADDR,
        .addr_bit_len = I2C_ADDR_BIT_LEN_7,
        .flags.enable_internal_pullup = 1,
    };
    esp_err_t err = i2c_new_slave_device(&cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C slave init failed: %s", esp_err_to_name(err));
        return err;
    }
    const i2c_slave_event_callbacks_t cbs = {
        .on_receive = on_receive,
        .on_request = on_request,
    };
    err = i2c_slave_register_event_callbacks(s_dev, &cbs, NULL);
    if (err != ESP_OK) {
        return err;
    }
    if (xTaskCreatePinnedToCore(ctrl_task, "ctrl_i2c", 4096, NULL, 5, NULL, 0) != pdPASS ||
        xTaskCreatePinnedToCore(status_task, "ctrl_status", 3072, NULL, 3, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "I2C slave 0x%02x on SDA GPIO%d / SCL GPIO%d, fw %u.%u.%u, protocol %u", CONFIG_AURACAST_I2C_ADDR,
             CONFIG_AURACAST_I2C_SDA_GPIO, CONFIG_AURACAST_I2C_SCL_GPIO, FW_VERSION[0], FW_VERSION[1], FW_VERSION[2],
             AURA_PROTO_VERSION);
#if CONFIG_AURACAST_AUTOSTART
    ESP_LOGW(TAG, "AUTOSTART: starting with the power-on configuration");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    handle_start();
    xSemaphoreGive(s_lock);
#else
    ESP_LOGI(TAG, "waiting for configuration and START");
#endif
    return ESP_OK;
}

void ctrl_i2c_wait_start(ctrl_config_t *out)
{
    xSemaphoreTake(s_started, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_lock);
}

void ctrl_i2c_set_state(uint8_t state, uint8_t err)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ctrl_regs_set_state(&s_regs, state, err, 0);
    xSemaphoreGive(s_lock);
}

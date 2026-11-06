#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_bluedroid_hci.h"
#include "esp_bt_main.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hci/hci_layer.h"
#include "stack/hcimsgs.h"
#include "wiimote_bt_trace.h"
#include "wiimote_bt_trace_packet.h"

#define TRACE_PACKET_BYTES 16
#define TRACE_QUEUE_LENGTH 32

typedef struct {
    int64_t time_us;
    uint16_t length;
    bool transmitted;
    uint8_t packet[TRACE_PACKET_BYTES];
} trace_record_t;

static const char *TAG = "wiimote_bt_trace";
static QueueHandle_t s_trace_queue;
static portMUX_TYPE s_counter_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_dropped;
static uint32_t s_malformed;
static esp_bluedroid_hci_driver_callbacks_t s_host_callbacks;
static esp_bluedroid_hci_driver_callbacks_t s_trace_callbacks;

/* Linker wrappers observe the existing IDF transport; they never replace packets. */
void __real_hci_host_send_packet(uint8_t *data, uint16_t length);
esp_err_t __real_hci_host_register_callback(
    const esp_bluedroid_hci_driver_callbacks_t *callback);

static void record_packet(bool transmitted, const uint8_t *data, uint16_t length)
{
    wiimote_bt_trace_filter_result_t result = wiimote_bt_trace_filter_packet(data, length);
    if (result == WIIMOTE_BT_TRACE_IGNORE) return;
    if (result == WIIMOTE_BT_TRACE_MALFORMED) {
        portENTER_CRITICAL(&s_counter_mux);
        ++s_malformed;
        portEXIT_CRITICAL(&s_counter_mux);
        return;
    }

    trace_record_t record = {
        .time_us = esp_timer_get_time(),
        .length = length,
        .transmitted = transmitted,
    };
    size_t captured_length = length < TRACE_PACKET_BYTES ? length : TRACE_PACKET_BYTES;
    memcpy(record.packet, data, captured_length);
    if (xQueueSend(s_trace_queue, &record, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_counter_mux);
        ++s_dropped;
        portEXIT_CRITICAL(&s_counter_mux);
    }
}

void __wrap_hci_host_send_packet(uint8_t *data, uint16_t length)
{
    record_packet(true, data, length);
    __real_hci_host_send_packet(data, length);
}

static int receive_packet(uint8_t *data, uint16_t length)
{
    record_packet(false, data, length);
    return s_host_callbacks.notify_host_recv(data, length);
}

esp_err_t __wrap_hci_host_register_callback(
    const esp_bluedroid_hci_driver_callbacks_t *callback)
{
    if (!callback) return __real_hci_host_register_callback(callback);
    s_host_callbacks = *callback;
    s_trace_callbacks = *callback;
    if (callback->notify_host_recv) s_trace_callbacks.notify_host_recv = receive_packet;
    return __real_hci_host_register_callback(&s_trace_callbacks);
}

static void log_packets(void *arg)
{
    (void)arg;
    static const char hex[] = "0123456789abcdef";
    trace_record_t record;
    for (;;) {
        if (xQueueReceive(s_trace_queue, &record, pdMS_TO_TICKS(1000)) == pdTRUE) {
            size_t captured_length =
                record.length < TRACE_PACKET_BYTES ? record.length : TRACE_PACKET_BYTES;
            char text[TRACE_PACKET_BYTES * 3 + 1];
            for (size_t i = 0; i < captured_length; ++i) {
                text[i * 3] = hex[record.packet[i] >> 4];
                text[i * 3 + 1] = hex[record.packet[i] & 0x0f];
                text[i * 3 + 2] = ' ';
            }
            text[captured_length * 3] = '\0';
            ESP_LOGI(TAG, "BT_HCI_%s at_us=%" PRId64 " len=%u packet=%s",
                     record.transmitted ? "TX" : "RX",
                     record.time_us, (unsigned)record.length, text);
        }

        portENTER_CRITICAL(&s_counter_mux);
        uint32_t dropped = s_dropped;
        uint32_t malformed = s_malformed;
        s_dropped = 0;
        s_malformed = 0;
        portEXIT_CRITICAL(&s_counter_mux);
        if (dropped || malformed) {
            ESP_LOGE(TAG, "Trace incomplete: dropped=%" PRIu32 " malformed=%" PRIu32,
                     dropped, malformed);
        }
    }
}

esp_err_t wiimote_bt_trace_start(void)
{
    if (s_trace_queue) {
        ESP_LOGE(TAG, "HCI trace already started");
        return ESP_ERR_INVALID_STATE;
    }
    s_trace_queue = xQueueCreate(TRACE_QUEUE_LENGTH, sizeof(trace_record_t));
    if (!s_trace_queue) {
        ESP_LOGE(TAG, "Could not allocate HCI trace queue");
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(log_packets, "wiimote_bt_trace", 3072, NULL,
                    tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        vQueueDelete(s_trace_queue);
        s_trace_queue = NULL;
        ESP_LOGE(TAG, "Could not create HCI trace logging task");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Discovery HCI trace enabled; PINs, link keys, and ACL payloads excluded");
    return ESP_OK;
}

esp_err_t wiimote_bt_trace_read_controller(void)
{
    if (!s_trace_queue || esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
        ESP_LOGE(TAG, "Cannot query controller before Bluedroid and HCI trace are ready");
        return ESP_ERR_INVALID_STATE;
    }

    static const uint16_t opcodes[] = {
        HCI_READ_CLASS_OF_DEVICE,
        HCI_READ_CURRENT_IAC_LAP,
        HCI_READ_NUM_SUPPORTED_IAC,
        HCI_READ_SCAN_ENABLE,
        HCI_READ_INQUIRYSCAN_CFG,
        HCI_READ_PAGESCAN_CFG,
        HCI_READ_INQSCAN_TYPE,
        HCI_READ_PAGESCAN_TYPE,
    };
    BT_HDR *commands[sizeof(opcodes) / sizeof(opcodes[0])];
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        commands[i] = HCI_GET_CMD_BUF(0);
        if (!commands[i]) {
            for (size_t j = 0; j < i; ++j) HCI_FREE_CMD_BUF(commands[j]);
            ESP_LOGE(TAG, "Could not allocate controller readback commands");
            return ESP_ERR_NO_MEM;
        }
        commands[i]->data[0] = (uint8_t)opcodes[i];
        commands[i]->data[1] = (uint8_t)(opcodes[i] >> 8);
        commands[i]->data[2] = 0;
        hci_cmd_metadata_t *metadata = HCI_GET_CMD_METAMSG(commands[i]);
        metadata->opcode = opcodes[i];
    }

    /* Use Bluedroid's command queue, not raw VHCI writes that bypass its credits. */
    const hci_t *hci = hci_layer_get_interface();
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        hci->transmit_command(commands[i], NULL, NULL, NULL);
    }
    ESP_LOGI(TAG, "Controller readback queued; inspect BT_HCI_RX completions, not BTM's cache");
    return ESP_OK;
}

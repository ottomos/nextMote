#include "wiimote_reports.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_err.h"
#include "esp_hidd_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "wiimote_protocol.h"
#include "wiimote_state.h"

static const char *TAG = "wiimote_reports";
static const uint8_t s_neutral_accelerometer[3] = {0x80, 0x80, 0x9a};
static ir_camera_state_t s_camera_state;
static portMUX_TYPE s_camera_state_mux = portMUX_INITIALIZER_UNLOCKED;

static void encode_mirrored_ir_points(uint8_t *output, const ir_camera_state_t *camera)
{
    for (size_t i = 0; i < IR_CAMERA_POINT_COUNT; ++i) {
        const size_t offset = i * 3;
        if (camera->x[i] >= IR_CAMERA_NO_POINT || camera->y[i] >= IR_CAMERA_HEIGHT) {
            memset(output + offset, 0xff, 3);
            continue;
        }

        const uint16_t x = (IR_CAMERA_WIDTH - 1) - camera->x[i];
        const uint16_t y = (IR_CAMERA_HEIGHT - 1) - camera->y[i];
        output[offset] = (uint8_t)x;
        output[offset + 1] = (uint8_t)y;
        output[offset + 2] =
            (uint8_t)(((x >> 4) & 0x30U) |
                      ((y >> 2) & 0xc0U) |
                      (camera->ir_report_data[offset + 2] & 0x0fU));
    }
}

void wiimote_reports_set_camera_state(const ir_camera_state_t *state)
{
    if (state == NULL) return;

    portENTER_CRITICAL(&s_camera_state_mux);
    bool changed = s_camera_state.detected != state->detected ||
                   s_camera_state.available != state->available ||
                   memcmp(s_camera_state.ir_report_data, state->ir_report_data,
                          sizeof(s_camera_state.ir_report_data)) != 0;
    s_camera_state = *state;
    portEXIT_CRITICAL(&s_camera_state_mux);

    wiimote_state_snapshot_t remote;
    wiimote_state_get_snapshot(&remote);
    if (remote.connected && remote.report_mode == WIIMOTE_REPORT_BUTTONS_ACCEL_IR &&
        remote.ir_logic_enabled && remote.ir_sensor_enabled &&
        (remote.continuous_reporting || changed)) {
        (void)wiimote_reports_send_current();
    }
}

uint8_t wiimote_reports_send_input(uint8_t report_id, const uint8_t *data, uint16_t length)
{
    if (!wiimote_state_is_connected()) return ESP_HID_PAR_HANDSHAKE_RSP_SUCCESS;

    esp_err_t err = esp_bt_hid_device_send_report(
        ESP_HIDD_REPORT_TYPE_INTRDATA, report_id, length, (uint8_t *)data);
    if (report_id == WIIMOTE_REPORT_BUTTONS_ACCEL_IR) {
        ESP_LOGD(TAG, "tx id=0x%02x length=%u status=%s",
                 report_id, length, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "tx id=0x%02x length=%u status=%s",
                 report_id, length, esp_err_to_name(err));
    }
    return err == ESP_OK ? ESP_HID_PAR_HANDSHAKE_RSP_SUCCESS :
                           ESP_HID_PAR_HANDSHAKE_RSP_NOT_READY;
}

uint8_t wiimote_reports_send_data(uint8_t report_id)
{
    uint8_t payload[WIIMOTE_REPORT_BUTTONS_ACCEL_IR_PAYLOAD_LEN] = {0};
    wiimote_state_get_buttons(payload);

    uint16_t length;
    switch (report_id) {
    case WIIMOTE_REPORT_BUTTONS:
        length = WIIMOTE_REPORT_BUTTONS_PAYLOAD_LEN;
        break;
    case WIIMOTE_REPORT_BUTTONS_ACCEL:
        memcpy(payload + WIIMOTE_BUTTON_PAYLOAD_LEN, s_neutral_accelerometer,
               sizeof(s_neutral_accelerometer));
        length = WIIMOTE_REPORT_BUTTONS_ACCEL_PAYLOAD_LEN;
        break;
    case WIIMOTE_REPORT_BUTTONS_ACCEL_IR:
        memcpy(payload + WIIMOTE_BUTTON_PAYLOAD_LEN, s_neutral_accelerometer,
               sizeof(s_neutral_accelerometer));
        {
            wiimote_state_snapshot_t remote;
            ir_camera_state_t camera;
            wiimote_state_get_snapshot(&remote);
            portENTER_CRITICAL(&s_camera_state_mux);
            camera = s_camera_state;
            portEXIT_CRITICAL(&s_camera_state_mux);

            uint8_t *ir_data = payload + WIIMOTE_REPORT_BUTTONS_ACCEL_PAYLOAD_LEN;
            if (remote.ir_logic_enabled && remote.ir_sensor_enabled &&
                camera.detected && camera.available) {
                encode_mirrored_ir_points(ir_data, &camera);
            } else {
                memset(ir_data, 0xff, WIIMOTE_REPORT_BUTTONS_ACCEL_IR_PAYLOAD_LEN -
                                          WIIMOTE_REPORT_BUTTONS_ACCEL_PAYLOAD_LEN);
            }
        }
        length = WIIMOTE_REPORT_BUTTONS_ACCEL_IR_PAYLOAD_LEN;
        break;
    default:
        ESP_LOGW(TAG, "Unsupported input report mode 0x%02x", report_id);
        return ESP_HID_PAR_HANDSHAKE_RSP_ERR_UNSUPPORTED_REQ;
    }

    return wiimote_reports_send_input(report_id, payload, length);
}

uint8_t wiimote_reports_send_current(void)
{
    uint8_t report_id;
    wiimote_state_get_report_mode(&report_id, NULL);
    if (report_id == 0) report_id = WIIMOTE_REPORT_BUTTONS;
    return wiimote_reports_send_data(report_id);
}

uint8_t wiimote_reports_send_status(void)
{
    wiimote_state_snapshot_t state;
    wiimote_state_get_snapshot(&state);

    const uint8_t payload[6] = {
        state.buttons[0],
        state.buttons[1],
        (uint8_t)(state.leds |
                  ((state.ir_logic_enabled && state.ir_sensor_enabled) ? 0x08U : 0U)),
        0,
        0,
        0xc0,
    };
    return wiimote_reports_send_input(WIIMOTE_REPORT_STATUS, payload, sizeof(payload));
}

void wiimote_reports_send_interrupt_ack(uint8_t report_id, uint8_t error)
{
    uint8_t buttons[WIIMOTE_BUTTON_PAYLOAD_LEN];
    wiimote_state_get_buttons(buttons);
    const uint8_t payload[4] = {
        buttons[0], buttons[1], report_id, error,
    };
    (void)wiimote_reports_send_input(WIIMOTE_REPORT_ACK, payload, sizeof(payload));
}

void wiimote_reports_send_memory_reply(uint8_t error, uint16_t address,
                                       const uint8_t *data, uint8_t length)
{
    uint8_t payload[WIIMOTE_MEMORY_REPORT_PAYLOAD_LEN] = {0};
    wiimote_state_get_buttons(payload);
    payload[2] = (uint8_t)(((length ? ((length - 1U) & 0x0fU) : 0U) << 4) |
                           (error & 0x0fU));
    payload[3] = (uint8_t)(address >> 8);
    payload[4] = (uint8_t)address;
    if (data != NULL && length > 0) memcpy(payload + 5, data, length);
    ESP_LOGI(TAG, "MEM_READ_REPLY addr=0x%04x size=%u error=%u",
             address, length, error);
    ESP_LOG_BUFFER_HEX(TAG, payload, sizeof(payload));
    (void)wiimote_reports_send_input(WIIMOTE_REPORT_READ_DATA_REPLY,
                                     payload, sizeof(payload));
}

void wiimote_reports_send_memory_error(uint8_t error, uint16_t address)
{
    uint8_t payload[WIIMOTE_MEMORY_REPORT_PAYLOAD_LEN] = {0};
    wiimote_state_get_buttons(payload);
    payload[2] = (uint8_t)(0xf0U | (error & 0x0fU));
    payload[3] = (uint8_t)(address >> 8);
    payload[4] = (uint8_t)address;
    ESP_LOGI(TAG, "MEM_READ_ERROR_REPLY addr=0x%04x error=%u size_minus_one=15",
             address, error);
    ESP_LOG_BUFFER_HEX(TAG, payload, sizeof(payload));
    (void)wiimote_reports_send_input(WIIMOTE_REPORT_READ_DATA_REPLY,
                                     payload, sizeof(payload));
}

void wiimote_reports_send_memory_write_ack(uint8_t error)
{
    uint8_t buttons[WIIMOTE_BUTTON_PAYLOAD_LEN];
    wiimote_state_get_buttons(buttons);
    const uint8_t payload[4] = {
        buttons[0], buttons[1], WIIMOTE_REPORT_WRITE_MEMORY, error,
    };
    (void)wiimote_reports_send_input(WIIMOTE_REPORT_WRITE_DATA_ACK,
                                     payload, sizeof(payload));
}

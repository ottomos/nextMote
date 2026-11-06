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
#include "freertos/task.h"
#include "imu_service.h"
#include "wiimote_accel.h"
#include "wiimote_motion_plus.h"
#include "wiimote_protocol.h"
#include "wiimote_state.h"

static const char *TAG = "wiimote_reports";
/* Temporary midpoint of the 0x00-0xff scale until battery voltage is measured. */
#define WIIMOTE_SYNTHETIC_BATTERY_HALF 0x80U
static ir_camera_state_t s_camera_state;
static portMUX_TYPE s_camera_state_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_counter_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_data_sent;
static uint32_t s_send_failures;

void wiimote_reports_get_counters(uint32_t *data_sent, uint32_t *send_failures)
{
    portENTER_CRITICAL(&s_counter_mux);
    *data_sent = s_data_sent;
    *send_failures = s_send_failures;
    portEXIT_CRITICAL(&s_counter_mux);
}

void wiimote_reports_note_send_failure(void)
{
    portENTER_CRITICAL(&s_counter_mux);
    ++s_send_failures;
    portEXIT_CRITICAL(&s_counter_mux);
}

static bool mirrored_ir_point(const ir_camera_state_t *camera, size_t i,
                              uint16_t *x, uint16_t *y);

static void encode_mirrored_ir_points(uint8_t *output, const ir_camera_state_t *camera)
{
    for (size_t i = 0; i < IR_CAMERA_POINT_COUNT; ++i) {
        const size_t offset = i * 3;
        uint16_t x, y;
        if (!mirrored_ir_point(camera, i, &x, &y)) {
            memset(output + offset, 0xff, 3);
            continue;
        }

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
    s_camera_state = *state;
    portEXIT_CRITICAL(&s_camera_state_mux);
}

uint8_t wiimote_reports_send_input(uint8_t report_id, const uint8_t *data, uint16_t length)
{
    if (!wiimote_state_is_connected()) return ESP_HID_PAR_HANDSHAKE_RSP_SUCCESS;

    esp_err_t err = esp_bt_hid_device_send_report(
        ESP_HIDD_REPORT_TYPE_INTRDATA, report_id, length, (uint8_t *)data);
    if (wiimote_protocol_report_has_sensor_data(report_id)) {
        if (err == ESP_OK) {
            portENTER_CRITICAL(&s_counter_mux);
            ++s_data_sent;
            portEXIT_CRITICAL(&s_counter_mux);
        }
        ESP_LOGD(TAG, "tx id=0x%02x length=%u status=%s",
                 report_id, length, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "tx id=0x%02x length=%u status=%s",
                 report_id, length, esp_err_to_name(err));
    }
    return err == ESP_OK ? ESP_HID_PAR_HANDSHAKE_RSP_SUCCESS :
                           ESP_HID_PAR_HANDSHAKE_RSP_NOT_READY;
}

static bool mirrored_ir_point(const ir_camera_state_t *camera, size_t i,
                              uint16_t *x, uint16_t *y)
{
    if (camera->x[i] >= IR_CAMERA_NO_POINT || camera->y[i] >= IR_CAMERA_HEIGHT) {
        *x = 0x3ffU;
        *y = 0x3ffU;
        return false;
    }
    *x = (IR_CAMERA_WIDTH - 1) - camera->x[i];
    *y = (IR_CAMERA_HEIGHT - 1) - camera->y[i];
    return true;
}

/* Basic IR format: two points per 5 bytes, used by the extension report modes. */
static void encode_basic_ir_points(uint8_t output[10], const ir_camera_state_t *camera)
{
    for (size_t pair = 0; pair < IR_CAMERA_POINT_COUNT / 2; ++pair) {
        uint16_t x1, y1, x2, y2;
        (void)mirrored_ir_point(camera, pair * 2, &x1, &y1);
        (void)mirrored_ir_point(camera, pair * 2 + 1, &x2, &y2);
        uint8_t *out = output + pair * 5;
        out[0] = (uint8_t)x1;
        out[1] = (uint8_t)y1;
        out[2] = (uint8_t)((((y1 >> 8) & 3U) << 6) | (((x1 >> 8) & 3U) << 4) |
                           (((y2 >> 8) & 3U) << 2) | ((x2 >> 8) & 3U));
        out[3] = (uint8_t)x2;
        out[4] = (uint8_t)y2;
    }
}

static bool camera_reportable(ir_camera_state_t *camera)
{
    wiimote_state_snapshot_t remote;
    wiimote_state_get_snapshot(&remote);
    portENTER_CRITICAL(&s_camera_state_mux);
    *camera = s_camera_state;
    portEXIT_CRITICAL(&s_camera_state_mux);
    return remote.ir_logic_enabled && remote.ir_sensor_enabled &&
           camera->detected && camera->available;
}

static void fill_accel(uint8_t *payload)
{
#if NEXTMOTE_SEND_IMU_TO_HID
    imu_state_t imu;
    imu_service_get_state(&imu);
    if (!imu.accel_available || !wiimote_accel_encode(payload, imu.accel_ms2)) {
        wiimote_accel_encode_neutral(payload);
    }
#else
    wiimote_accel_encode_neutral(payload);
#endif
}

static void fill_ir(uint8_t *output, size_t length)
{
    ir_camera_state_t camera;
    if (!camera_reportable(&camera)) {
        memset(output, 0xff, length);
    } else if (length == 10) {
        encode_basic_ir_points(output, &camera);
    } else {
        encode_mirrored_ir_points(output, &camera);
    }
}

static void fill_extension(uint8_t *output, size_t length)
{
    memset(output, 0, length);
    if (wiimote_motion_plus_is_active() && length >= WIIMOTE_MOTION_PLUS_DATA_LEN) {
        imu_state_t imu;
        imu_service_get_state(&imu);
        float rates[3] = {0};
        if (imu.gyro_available) wiimote_accel_map_gyro(imu.gyro_rad_s, rates);
        wiimote_motion_plus_encode(output, rates, imu.gyro_available);
    }
}

/* Layouts per mode: B=buttons(2) A=accel(3) I=IR E=extension. */
static uint16_t build_data_payload(uint8_t report_id, uint8_t *payload)
{
    if (report_id == WIIMOTE_REPORT_EXT21) {
        fill_extension(payload, 21);
        return 21;
    }

    wiimote_state_get_buttons(payload);
    switch (report_id) {
    case WIIMOTE_REPORT_BUTTONS:
        return WIIMOTE_REPORT_BUTTONS_PAYLOAD_LEN;
    case WIIMOTE_REPORT_BUTTONS_ACCEL:
        fill_accel(payload);
        return WIIMOTE_REPORT_BUTTONS_ACCEL_PAYLOAD_LEN;
    case WIIMOTE_REPORT_BUTTONS_EXT8:
        fill_extension(payload + 2, 8);
        return 10;
    case WIIMOTE_REPORT_BUTTONS_ACCEL_IR:
        fill_accel(payload);
        fill_ir(payload + 5, 12);
        return WIIMOTE_REPORT_BUTTONS_ACCEL_IR_PAYLOAD_LEN;
    case WIIMOTE_REPORT_BUTTONS_EXT19:
        fill_extension(payload + 2, 19);
        return 21;
    case WIIMOTE_REPORT_BUTTONS_ACCEL_EXT16:
        fill_accel(payload);
        fill_extension(payload + 5, 16);
        return 21;
    case WIIMOTE_REPORT_BUTTONS_IR10_EXT9:
        fill_ir(payload + 2, 10);
        fill_extension(payload + 12, 9);
        return 21;
    case WIIMOTE_REPORT_BUTTONS_ACCEL_IR10_EXT6:
        fill_accel(payload);
        fill_ir(payload + 5, 10);
        fill_extension(payload + 15, 6);
        return 21;
    default:
        return 0;
    }
}

uint8_t wiimote_reports_send_data(uint8_t report_id)
{
    uint8_t payload[WIIMOTE_REPORT_MAX_PAYLOAD_LEN] = {0};
    uint16_t length = build_data_payload(report_id, payload);
    if (length == 0) {
        ESP_LOGW(TAG, "Unsupported input report mode 0x%02x", report_id);
        return ESP_HID_PAR_HANDSHAKE_RSP_ERR_UNSUPPORTED_REQ;
    }
    return wiimote_reports_send_input(report_id, payload, length);
}

static void sensor_report_task(void *arg)
{
    (void)arg;
    uint8_t previous[WIIMOTE_REPORT_MAX_PAYLOAD_LEN] = {0};
    uint8_t previous_mode = 0;
    bool accel_was_live = false;
    bool send_failed = false;
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        imu_state_t imu;
        imu_service_get_state(&imu);
        if (imu.accel_available != accel_was_live) {
            if (imu.accel_available) {
                ESP_LOGI(TAG, "HID accelerometer source: live BNO08x");
            } else {
                ESP_LOGW(TAG, "HID accelerometer source: neutral fallback (missing, invalid, or stale data)");
            }
            accel_was_live = imu.accel_available;
        }

        wiimote_state_snapshot_t remote;
        wiimote_state_get_snapshot(&remote);
        if (remote.connected &&
            wiimote_protocol_report_has_sensor_data(remote.report_mode)) {
            uint8_t payload[sizeof(previous)] = {0};
            uint16_t length = build_data_payload(remote.report_mode, payload);
            if (remote.continuous_reporting || previous_mode != remote.report_mode ||
                memcmp(previous, payload, length) != 0) {
                uint8_t result = wiimote_reports_send_input(remote.report_mode, payload, length);
                if (result == ESP_HID_PAR_HANDSHAKE_RSP_SUCCESS) {
                    memcpy(previous, payload, length);
                    previous_mode = remote.report_mode;
                    if (send_failed) ESP_LOGI(TAG, "Sensor HID report sends recovered");
                    send_failed = false;
                } else if (!send_failed) {
                    ESP_LOGW(TAG, "Sensor HID report send failed; retrying on next tick");
                    send_failed = true;
                }
            }
        } else {
            previous_mode = 0;
        }
        /* MotionPlus integrates gyro rates, so feed it at the real remote's ~100 Hz. */
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(wiimote_motion_plus_is_active() ? 10 : 20));
    }
}

esp_err_t wiimote_reports_start(void)
{
    BaseType_t result = xTaskCreate(sensor_report_task, "sensor_reports", 3072,
                                    NULL, 5, NULL);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create sensor HID reporting task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
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
                  (wiimote_motion_plus_is_active() ? 0x02U : 0U) |
                  ((state.ir_logic_enabled && state.ir_sensor_enabled) ? 0x08U : 0U)),
        0,
        0,
        WIIMOTE_SYNTHETIC_BATTERY_HALF,
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

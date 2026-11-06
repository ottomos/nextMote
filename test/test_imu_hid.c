#include <assert.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_hidd_api.h"
#include "freertos/task.h"
#include "imu_service.h"
#include "wiimote_accel.h"
#include "wiimote_reports.h"
#include "wiimote_state.h"

static imu_state_t imu;
static uint8_t received[32];
static uint8_t received_id;
static uint16_t received_length;
static unsigned sends;
static esp_err_t send_result;
static void (*report_task)(void *);
static jmp_buf task_exit;
static unsigned ticks_left;
static void (*tick_action)(void);

const char *esp_err_to_name(esp_err_t err) { return err ? "FAIL" : "OK"; }
void host_test_log(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}
void imu_service_get_state(imu_state_t *state) { *state = imu; }
esp_err_t esp_bt_hid_device_send_report(int type, uint8_t id, uint16_t length,
                                       uint8_t *payload)
{
    (void)type;
    assert(length <= sizeof(received));
    received_id = id;
    received_length = length;
    memcpy(received, payload, length);
    ++sends;
    return send_result;
}
BaseType_t xTaskCreate(void (*entry)(void *), const char *name, unsigned stack,
                      void *arg, unsigned priority, void *handle)
{
    (void)name; (void)stack; (void)arg; (void)priority; (void)handle;
    report_task = entry;
    return pdPASS;
}
TickType_t xTaskGetTickCount(void) { return 0; }
void vTaskDelayUntil(TickType_t *last, TickType_t period)
{
    assert(period == pdMS_TO_TICKS(20));
    *last += period;
    if (tick_action != NULL) tick_action();
    if (--ticks_left == 0) longjmp(task_exit, 1);
}
static void run_ticks(unsigned count)
{
    ticks_left = count;
    if (setjmp(task_exit) == 0) report_task(NULL);
}
static unsigned decoded_axis(const uint8_t *payload, unsigned axis)
{
    if (axis == 0) return (payload[2] << 2) | ((payload[0] >> 5) & 3);
    return (payload[2 + axis] << 2) |
           ((payload[1] >> (axis == 1 ? 4 : 5)) & 2);
}
static void test_encoding(void)
{
    const unsigned axes[] = {WIIMOTE_ACCEL_X_AXIS, WIIMOTE_ACCEL_Y_AXIS, WIIMOTE_ACCEL_Z_AXIS};
    const int signs[] = {WIIMOTE_ACCEL_X_SIGN, WIIMOTE_ACCEL_Y_SIGN, WIIMOTE_ACCEL_Z_SIGN};
    for (unsigned axis = 0; axis < 3; ++axis) {
        for (int sign = -1; sign <= 1; ++sign) {
            float input[3] = {0};
            input[axes[axis]] = signs[axis] * sign * WIIMOTE_ACCEL_GRAVITY_MS2;
            uint8_t data[5] = {0x1f, 0x9f};
            assert(wiimote_accel_encode(data, input));
            for (unsigned i = 0; i < 3; ++i) {
                assert(decoded_axis(data, i) == (unsigned)(512 + (i == axis ? sign * 104 : 0)));
            }
            assert((data[0] & ~0x60) == 0x1f);
            assert((data[1] & ~0x60) == 0x9f);
        }
    }
    /* Exercise every representable value and every packed low-bit combination. */
    for (unsigned raw = 0; raw < 1024; ++raw) {
        float input[3] = {0};
        for (unsigned i = 0; i < 3; ++i) {
            input[axes[i]] = signs[i] * ((int)raw - 512) *
                             WIIMOTE_ACCEL_GRAVITY_MS2 / 104.0f;
        }
        uint8_t data[5] = {0xff, 0xff};
        assert(wiimote_accel_encode(data, input));
        assert(decoded_axis(data, 0) == raw);
        assert(decoded_axis(data, 1) == (raw & ~1U));
        assert(decoded_axis(data, 2) == (raw & ~1U));
        assert((data[0] & ~0x60) == 0x9f);
        assert((data[1] & ~0x60) == 0x9f);
    }
    float input[3] = {INFINITY, 0, 0};
    uint8_t data[5] = {0xff, 0xff, 1, 2, 3};
    const uint8_t original[5] = {0xff, 0xff, 1, 2, 3};
    assert(!wiimote_accel_encode(data, input));
    assert(memcmp(data, original, 5) == 0);
    input[0] = NAN;
    assert(!wiimote_accel_encode(data, input));
    input[0] = 1000000; input[1] = -1000000; input[2] = 1000000;
    assert(wiimote_accel_encode(data, input));
    for (unsigned i = 0; i < 3; ++i) {
        float mapped = input[axes[i]] * signs[i];
        assert(decoded_axis(data, i) == (mapped < 0 ? 0U : (i == 0 ? 1023U : 1022U)));
    }
    wiimote_accel_encode_neutral(data);
    assert(data[0] == 0x9f && data[1] == 0x9f);
    assert(decoded_axis(data, 0) == 512 && decoded_axis(data, 1) == 512 &&
           decoded_axis(data, 2) == 616);
}
static void test_reports(void)
{
    wiimote_state_open();
    assert(wiimote_state_set_button_mask(0x198c));
    imu.accel_available = true;
    imu.accel_ms2[0] = 2.3f; imu.accel_ms2[1] = -4.1f; imu.accel_ms2[2] = 9.1f;
    uint8_t expected[5] = {0x19, 0x8c};
    assert(wiimote_accel_encode(expected, imu.accel_ms2));
    assert(wiimote_reports_send_data(0x31) == 0);
    assert(received_id == 0x31 && received_length == 5);
    assert(memcmp(received, expected, 5) == 0);
    wiimote_reports_send_data(0x30);
    assert(received_length == 2 && received[0] == 0x19 && received[1] == 0x8c);
    wiimote_reports_send_data(0x33);
    assert(received_id == 0x33 && received_length == 17);
    assert(memcmp(received, expected, 5) == 0);
    for (unsigned i = 5; i < 17; ++i) assert(received[i] == 0xff);

    ir_camera_state_t camera = {.detected = true, .available = true};
    for (unsigned i = 0; i < 4; ++i) { camera.x[i] = 100; camera.y[i] = 200; }
    wiimote_reports_set_camera_state(&camera);
    wiimote_state_set_ir_feature(0x13, true);
    wiimote_state_set_ir_feature(0x1a, true);
    wiimote_reports_send_data(0x33);
    assert(memcmp(received, expected, 5) == 0);
    assert(received[5] == (uint8_t)923 && received[6] == (uint8_t)567);
    assert(received[7] == 0xb0);
    imu.accel_available = false;
    wiimote_reports_send_data(0x31);
    assert(received[2] == 0x80 && received[3] == 0x80 && received[4] == 0x9a);
    assert(received[0] == 0x19 && received[1] == 0x8c);
    imu.accel_available = true;
    imu.accel_ms2[0] = NAN;
    wiimote_reports_send_data(0x31);
    assert(received[2] == 0x80 && received[3] == 0x80 && received[4] == 0x9a);
    imu.accel_available = false;
    wiimote_reports_send_status();
    assert(received_id == 0x20 && received[0] == 0x19 && received[1] == 0x8c);
    wiimote_reports_send_memory_write_ack(0);
    assert(received_id == 0x22 && received[0] == 0x19 && received[1] == 0x8c);
    assert(wiimote_reports_send_data(0x37) == ESP_HID_PAR_HANDSHAKE_RSP_ERR_UNSUPPORTED_REQ);
    wiimote_state_close();
    unsigned before = sends;
    wiimote_reports_send_data(0x31);
    assert(sends == before);
}
static void change_sample(void)
{
    imu.accel_available = true;
    imu.accel_ms2[0] += 1.0f;
    imu.accel_ms2[1] = 0;
    imu.accel_ms2[2] = 0;
}
static void test_cadence(void)
{
    assert(wiimote_reports_start() == ESP_OK);
    wiimote_state_open();
    for (unsigned mode = 0x31; mode <= 0x33; mode += 2) {
        wiimote_state_set_report_mode(mode, true);
        sends = 0; run_ticks(50);
        assert(sends == 50);
        wiimote_state_set_report_mode(mode, false);
        sends = 0; run_ticks(50);
        assert(sends == 1);
    }
    wiimote_state_set_report_mode(0x30, true);
    sends = 0; run_ticks(5);
    assert(sends == 0);
    wiimote_state_set_report_mode(0x31, false);
    send_result = ESP_FAIL;
    sends = 0; run_ticks(3);
    assert(sends == 3);
    send_result = ESP_OK;
    imu.accel_ms2[0] = 0;
    tick_action = change_sample;
    sends = 0; run_ticks(3);
    assert(sends == 3);
    tick_action = NULL;
    wiimote_state_close();
    sends = 0; run_ticks(5);
    assert(sends == 0);
}
static void test_freshness(void)
{
    imu_state_t state = {.accel_available = true, .accel_time_us = 1000000};
    assert(imu_service_accel_is_fresh(&state, 1000000));
    assert(imu_service_accel_is_fresh(&state, 1250000));
    assert(!imu_service_accel_is_fresh(&state, 1250001));
    assert(!imu_service_accel_is_fresh(&state, 999999));
    state.accel_available = false;
    assert(!imu_service_accel_is_fresh(&state, 1000000));
}
int main(void)
{
    test_freshness();
    test_encoding();
    test_reports();
    test_cadence();
    puts("IMU HID encoding, report integration, IR/buttons, fallback and cadence: PASS");
}

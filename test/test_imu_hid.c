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
#include "wiimote_motion_plus.h"
#include "wiimote_nunchuk.h"
#include "wiimote_protocol.h"
#include "wiimote_reports.h"
#include "wiimote_state.h"

static imu_state_t imu;
int64_t host_test_time_us;
static TickType_t expected_period = 20;
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
    assert(period == pdMS_TO_TICKS(expected_period));
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
    assert(wiimote_reports_send_data(0x37) == 0);
    assert(received_id == 0x37 && received_length == 21);
    for (unsigned i = 15; i < 21; ++i) assert(received[i] == 0);
    wiimote_reports_send_data(0x36);
    assert(received_length == 21 && received[0] == 0x19);
    /* Basic IR: x=923 (0x39b), y=567 (0x237) for both points of each pair. */
    assert(received[2] == 0x9b && received[3] == 0x37 && received[4] == 0xbb);
    assert(received[5] == 0x9b && received[6] == 0x37);
    wiimote_reports_send_data(0x3d);
    assert(received_id == 0x3d && received_length == 21);
    assert(wiimote_reports_send_data(0x3e) == ESP_HID_PAR_HANDSHAKE_RSP_ERR_UNSUPPORTED_REQ);
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
static int decode_mp(const uint8_t *d, unsigned axis, bool *slow)
{
    /* axis: 0 yaw, 1 roll, 2 pitch */
    const unsigned value = d[axis] | ((d[3 + axis] >> 2) << 8);
    if (axis == 0) *slow = d[3] & 2;
    else if (axis == 1) *slow = d[4] & 2;
    else *slow = d[3] & 1;
    return (int)value - 8192;
}
static void test_motion_plus(void)
{
    uint8_t out[16];
    uint8_t error = 0xff;
    bool changed = true;
    wiimote_motion_plus_reset();
    assert(!wiimote_motion_plus_is_active());
    assert(wiimote_motion_plus_read(0x53, 0xfa, 6, out, &error) && error == 0);
    const uint8_t inactive_id[6] = {0, 0, 0xa6, 0x20, 0, 5};
    assert(memcmp(out, inactive_id, 6) == 0);
    assert(!wiimote_motion_plus_read(0x52, 0xfa, 6, out, &error));
    assert(wiimote_motion_plus_read(0x53, 0x20, 16, out, &error) && error == 0);
    assert(out[0] == 0x80 && out[1] == 0x00);

    host_test_time_us = 1000000;
    const uint8_t mode = 0x04;
    assert(wiimote_motion_plus_write(0x53, 0xfe, &mode, 1, &error, &changed));
    assert(error == 0 && changed && wiimote_motion_plus_is_active());
    assert(wiimote_motion_plus_read(0x53, 0xfa, 6, out, &error) && error != 0);
    assert(wiimote_motion_plus_read(0x52, 0xfa, 6, out, &error) && error == 0);
    const uint8_t active_id[6] = {0, 0, 0xa4, 0x20, 4, 5};
    assert(memcmp(out, active_id, 6) == 0);
    assert(wiimote_motion_plus_read(0x52, 0xf7, 1, out, &error) && out[0] == 0x02);
    host_test_time_us += 500000;
    assert(wiimote_motion_plus_read(0x52, 0xf7, 1, out, &error) && out[0] == 0x0e);
    const uint8_t type = 0;
    assert(wiimote_motion_plus_write(0x52, 0xf1, &type, 1, &error, &changed) && !changed);
    assert(wiimote_motion_plus_read(0x52, 0xf7, 1, out, &error) && out[0] == 0x14);
    host_test_time_us += 30000;
    assert(wiimote_motion_plus_read(0x52, 0xf7, 1, out, &error) && out[0] == 0x1a);

    wiimote_state_open();
    wiimote_reports_send_status();
    assert(received_id == 0x20 && (received[2] & 0x02));
    imu.gyro_available = true;
#if WIIMOTE_ACCEL_X_AXIS == 1
    imu.gyro_rad_s[0] = 0; imu.gyro_rad_s[1] = -1.0f; imu.gyro_rad_s[2] = -20.0f;
#else
    imu.gyro_rad_s[0] = 1.0f; imu.gyro_rad_s[1] = 0; imu.gyro_rad_s[2] = -20.0f;
#endif
    wiimote_reports_send_data(0x35);
    assert(received_id == 0x35 && received_length == 21);
    bool slow;
    const uint8_t *mp = received + 5;
    assert(mp[5] & 0x02);
    int pitch = decode_mp(mp, 2, &slow);
    assert(slow && pitch > 900 && pitch < 950);
    int yaw = decode_mp(mp, 0, &slow);
    assert(!slow && yaw < -4000 && yaw > -4300);
    assert(decode_mp(mp, 1, &slow) == 0 && slow);
    imu.gyro_available = false;
    wiimote_reports_send_data(0x37);
    mp = received + 15;
    assert(decode_mp(mp, 0, &slow) == 0 && decode_mp(mp, 2, &slow) == 0);

    assert(wiimote_reports_start() == ESP_OK);
    wiimote_state_set_report_mode(0x35, true);
    expected_period = 10;
    sends = 0; run_ticks(20);
    assert(sends == 20);

    const uint8_t init = 0x55;
    assert(wiimote_motion_plus_write(0x52, 0xf0, &init, 1, &error, &changed));
    assert(changed && !wiimote_motion_plus_is_active());
    expected_period = 20;
    sends = 0; run_ticks(5);
    assert(sends == 5);
    wiimote_reports_send_status();
    assert(!(received[2] & 0x02));
    wiimote_state_close();

    float mapped[3];
    const float sensor[3] = {1, 2, 3};
    wiimote_accel_map_gyro(sensor, mapped);
#if WIIMOTE_ACCEL_X_AXIS == 1
    /* x<-(-y), y<-x: a rotation about z (det +1), so no extra flip. */
    assert(mapped[0] == -2 && mapped[1] == 1 && mapped[2] == 3);
#else
    assert(mapped[0] == 1 && mapped[1] == 2 && mapped[2] == 3);
#endif
}
static uint8_t nc_decrypt(uint8_t value) { return (uint8_t)((value ^ 0x97) + 0x97); }
static void test_nunchuk(void)
{
    uint8_t out[16];
    uint8_t error = 0xff;
    wiimote_motion_plus_reset();
    wiimote_nunchuk_reset();
    assert(!wiimote_nunchuk_is_attached());
    assert(!wiimote_nunchuk_read(0x52, 0xfa, 6, out, &error));
    const uint8_t init = 0x55;
    assert(!wiimote_nunchuk_write(0x52, 0xf0, &init, 1, &error));

    wiimote_state_open();
    wiimote_state_set_report_mode(0x35, true);
    sends = 0;
    assert(wiimote_reports_set_nunchuk_attached(true));
    assert(!wiimote_reports_set_nunchuk_attached(true));
    assert(sends == 1 && received_id == 0x20 && (received[2] & 0x02));
    uint8_t mode = 0xff;
    bool continuous = false;
    wiimote_state_get_report_mode(&mode, &continuous);
    assert(mode == 0 && continuous);

    assert(!wiimote_nunchuk_read(0x53, 0xfa, 6, out, &error));
    assert(wiimote_nunchuk_write(0x52, 0xf0, &init, 1, &error) && error == 0);
    const uint8_t type = 0;
    assert(wiimote_nunchuk_write(0x52, 0xfb, &type, 1, &error) && error == 0);
    assert(wiimote_nunchuk_read(0x52, 0xfa, 6, out, &error) && error == 0);
    const uint8_t id[6] = {0, 0, 0xa4, 0x20, 0, 0};
    assert(memcmp(out, id, 6) == 0);
    assert(wiimote_nunchuk_read(0x52, 0x01fa, 6, out, &error) && error == 0);
    assert(memcmp(out, id, 6) == 0);
    assert(wiimote_nunchuk_read(0x52, 0x20, 16, out, &error) && error == 0);
    uint8_t checksum = 0x55;
    for (int i = 0; i < 14; ++i) checksum = (uint8_t)(checksum + out[i]);
    assert(out[14] == checksum && out[15] == (uint8_t)(checksum + 0x55));
    assert(out[0] == 0x80 && out[4] == 0xb3 && out[8] == 0xe0 && out[9] == 0x20 && out[10] == 0x80);
    uint8_t mirror[16];
    assert(wiimote_nunchuk_read(0x52, 0x30, 16, mirror, &error) && memcmp(out, mirror, 16) == 0);
    assert(wiimote_nunchuk_read(0x52, 0xff, 2, out, &error) && error != 0);

    wiimote_nunchuk_input_t input;
    wiimote_nunchuk_neutral_input(&input);
    wiimote_reports_send_data(0x35);
    const uint8_t *ext = received + 5;
    assert(ext[0] == 0x80 && ext[1] == 0x80 && ext[2] == 0x80 && ext[4] == 0xb3);
    assert((ext[5] & 0x03) == 0x03);
    input.stick_x = 0x20; input.stick_y = 0xe0;
    input.accel[0] = 0x201; input.accel[1] = 0x202; input.accel[2] = 0x2cf;
    input.c = true;
    wiimote_nunchuk_set_input(&input);
    wiimote_reports_send_data(0x35);
    assert(ext[0] == 0x20 && ext[1] == 0xe0 && ext[2] == 0x80 && ext[3] == 0x80 && ext[4] == 0xb3);
    assert(ext[5] == ((3 << 6) | (2 << 4) | (1 << 2) | 0x01));
    input.c = false; input.z = true;
    wiimote_nunchuk_set_input(&input);
    wiimote_reports_send_data(0x32);
    assert(received_length == 10 && (received[2 + 5] & 0x03) == 0x02);
    wiimote_reports_send_data(0x3d);
    assert(received[0] == 0x20 && received[1] == 0xe0 && received[6] == 0);

    const uint8_t zero_key[16] = {0};
    assert(wiimote_nunchuk_write(0x52, 0x40, zero_key, 16, &error));
    const uint8_t enable = 0xaa;
    assert(wiimote_nunchuk_write(0x52, 0xf0, &enable, 1, &error));
    wiimote_nunchuk_status_t status;
    wiimote_nunchuk_get_status(&status);
    assert(status.encrypted && !status.unsupported_key && status.encryption_enables == 1);
    wiimote_reports_send_data(0x35);
    assert(nc_decrypt(ext[0]) == 0x20 && nc_decrypt(ext[1]) == 0xe0 && nc_decrypt(ext[4]) == 0xb3);
    assert(nc_decrypt(ext[6]) == 0);
    assert(wiimote_nunchuk_read(0x52, 0xfa, 6, out, &error));
    for (int i = 0; i < 6; ++i) assert(nc_decrypt(out[i]) == id[i]);
    const uint8_t key_byte = 0x12;
    assert(wiimote_nunchuk_write(0x52, 0x45, &key_byte, 1, &error));
    wiimote_reports_send_data(0x35);
    assert(ext[0] == 0x20);
    wiimote_nunchuk_get_status(&status);
    assert(status.unsupported_key);

    sends = 0;
    assert(wiimote_reports_set_nunchuk_attached(false));
    assert(sends == 1 && received_id == 0x20 && !(received[2] & 0x02));
    assert(!wiimote_nunchuk_read(0x52, 0xfa, 6, out, &error));
    wiimote_reports_send_data(0x35);
    for (int i = 0; i < 16; ++i) assert(received[5 + i] == 0);
    assert(wiimote_reports_set_nunchuk_attached(true));
    wiimote_nunchuk_get_status(&status);
    assert(!status.encrypted && !status.unsupported_key);

    /* MotionPlus with the Nunchuk on its passthrough port. */
    wiimote_motion_plus_reset();
    bool changed = true;
    const uint8_t plain_mode = 0x04;
    assert(wiimote_motion_plus_write(0x53, 0xfe, &plain_mode, 1, &error, &changed));
    assert(!changed && wiimote_motion_plus_mode() == 0x04);
    imu.gyro_available = false;
    for (int i = 0; i < 2; ++i) {
        wiimote_reports_send_data(0x35);
        assert((ext[5] & 0x02) && (ext[4] & 0x01));
    }
    assert(wiimote_motion_plus_write(0x52, 0xf0, &init, 1, &error, &changed) && changed);
    const uint8_t pass_mode = 0x05;
    assert(wiimote_motion_plus_write(0x53, 0xfe, &pass_mode, 1, &error, &changed));
    assert(!changed && wiimote_motion_plus_mode() == 0x05);
    input.stick_x = 0x31; input.stick_y = 0xc2;
    input.accel[0] = 0x2ab; input.accel[1] = 0x156; input.accel[2] = 0x3fd;
    input.c = true; input.z = false;
    wiimote_nunchuk_set_input(&input);
    unsigned mp_frames = 0, nc_frames = 0;
    for (int i = 0; i < 6; ++i) {
        wiimote_reports_send_data(0x37);
        const uint8_t *e = received + 15;
        assert(e[4] & 0x01);
        if (e[5] & 0x02) {
            ++mp_frames;
            continue;
        }
        ++nc_frames;
        assert(e[0] == 0x31 && e[1] == 0xc2 && (e[5] & 0x03) == 0);
        const unsigned ax = (unsigned)(e[2] << 2) | ((e[5] >> 3) & 2);
        const unsigned ay = (unsigned)(e[3] << 2) | ((e[5] >> 4) & 2);
        const unsigned az = (unsigned)((e[4] & 0xfe) << 2) | ((e[5] >> 5) & 6);
        assert(ax == (0x2ab & ~1U) && ay == (0x156 & ~1U) && az == (0x3fd & ~1U));
        assert(!(e[5] & 0x08) && (e[5] & 0x04)); /* C pressed, Z released */
    }
    assert(mp_frames == 3 && nc_frames == 3);
    /* The M+ mirrors the Nunchuk ID and plaintext calibration (WSR checks these). */
    uint8_t nc_calib[16];
    assert(wiimote_nunchuk_write(0x52, 0xf0, &enable, 1, &error)); /* encryption must not leak */
    uint8_t nc_id[6];
    assert(wiimote_nunchuk_passthrough_info(nc_id, nc_calib));
    assert(wiimote_motion_plus_read(0x52, 0x40, 16, out, &error) && error == 0);
    assert(memcmp(out, nc_calib, 16) == 0);
    assert((uint8_t)(out[14]) == (uint8_t)(0x55 + out[0] + out[1] + out[2] + out[3] + out[4] +
                                           out[5] + out[6] + out[7] + out[8] + out[9] +
                                           out[10] + out[11] + out[12] + out[13]));
    assert(wiimote_motion_plus_read(0x52, 0xf6, 4, out, &error) && error == 0);
    assert(out[0] == 0x00 && out[2] == 0x00 && out[3] == 0x00); /* Nunchuk ID bytes 4, 0, 5 */
    wiimote_nunchuk_set_attached(false);
    assert(wiimote_motion_plus_read(0x52, 0x40, 16, out, &error) && out[14] == 0 && out[15] == 0);
    for (int i = 0; i < 2; ++i) {
        wiimote_reports_send_data(0x37);
        assert((received[15 + 5] & 0x02) && !(received[15 + 4] & 0x01));
    }
    wiimote_motion_plus_reset();
    wiimote_state_close();
    sends = 0;
    assert(!wiimote_reports_set_nunchuk_attached(false));
    assert(sends == 0);
}
int main(void)
{
    test_freshness();
    test_encoding();
    test_reports();
    test_cadence();
    test_motion_plus();
    test_nunchuk();
    puts("IMU HID encoding, report integration, IR/buttons, fallback, cadence, MotionPlus and Nunchuk: PASS");
}

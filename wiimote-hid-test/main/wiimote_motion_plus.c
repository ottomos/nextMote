#include "wiimote_motion_plus.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "wiimote_nunchuk.h"
#include "wiimote_protocol.h"

/*
 * Emulated Wii MotionPlus, following the register model documented on WiiBrew
 * and implemented by Dolphin's emulated M+:
 *  - inactive: answers at I2C 0x53 with ID 00 00 a6 20 00 05;
 *  - a write of 0x04/0x05/0x07 to 0xfe activates it at 0x52 (ID a4 20 0x 05);
 *  - any write to 0xf0 while active deactivates it again;
 *  - 0x50..0x8f carries the "challenge" values, 0xf7 its progress state.
 * The challenge values below are x = r^2 mod n, y0 = r and y1 = r*sqrt(v) mod n
 * for a fixed r, precomputed offline (little endian, 64 bytes).
 */

static const char *TAG = "motion_plus";

#define MP_REG_DATA 0x00
#define MP_REG_CALIBRATION 0x20
#define MP_REG_CHALLENGE 0x50
#define MP_REG_INIT 0xf0
#define MP_REG_CHALLENGE_TYPE 0xf1
#define MP_REG_CHALLENGE_STATE 0xf7
#define MP_REG_ID 0xfa
#define MP_REG_MODE 0xfe
#define MP_CHALLENGE_LEN 64

#define MP_STATE_PREPARING_X 0x02
#define MP_STATE_X_READY 0x0e
#define MP_STATE_PREPARING_Y 0x14
#define MP_STATE_Y_READY 0x1a

/* The SDK needs at least one "not ready" read before x becomes ready. */
#define MP_PREPARE_X_US 500000
#define MP_PREPARE_Y0_US 30000
#define MP_PREPARE_Y1_US 500000

/* Raw 14-bit encoding: zero 8192; scales match the calibration block below. */
#define MP_ZERO_VALUE 8192
#define MP_MAX_VALUE 16383
#define MP_SLOW_PER_RAD_S 923.53f
#define MP_FAST_PER_RAD_S 207.79f

static const uint8_t s_mp_calibration[32] = {
    0x80, 0x00, 0x80, 0x00, 0x80, 0x00, 0x3c, 0x00, 0xc4, 0x00, 0x3c, 0x00,
    0xc8, 0x0b, 0x3f, 0x4f, 0x80, 0x00, 0x80, 0x00, 0x80, 0x00, 0x3c, 0x00,
    0xc4, 0x00, 0x3c, 0x00, 0x2d, 0xe9, 0xc9, 0xc1,
};
static const uint8_t s_mp_challenge_x[MP_CHALLENGE_LEN] = {
    0xd9, 0x6d, 0x8e, 0xb6, 0x6a, 0xc2, 0x1e, 0x4f, 0x45, 0x6d, 0x75, 0xae,
    0x48, 0xeb, 0xfd, 0x93, 0x95, 0xe9, 0x3e, 0x5e, 0x0c, 0x77, 0x18, 0x5b,
    0x8b, 0x07, 0x6c, 0x25, 0x1f, 0xc1, 0x0a, 0x98, 0x47, 0xe9, 0xf8, 0xfa,
    0xd0, 0x2f, 0x94, 0x92, 0x3d, 0x94, 0x47, 0x40, 0x31, 0xda, 0x40, 0xf2,
    0xb8, 0x28, 0x36, 0x81, 0x67, 0x1f, 0xfe, 0x36, 0xbc, 0x5b, 0x9b, 0x2f,
    0x00, 0x00, 0x00, 0x00,
};
static const uint8_t s_mp_challenge_y0[MP_CHALLENGE_LEN] = {
    0x65, 0x67, 0x6e, 0x65, 0x6c, 0x6c, 0x61, 0x68, 0x63, 0x20, 0x73, 0x75,
    0x6c, 0x70, 0x20, 0x6e, 0x6f, 0x69, 0x74, 0x6f, 0x6d, 0x20, 0x65, 0x74,
    0x6f, 0x4d, 0x74, 0x78, 0x65, 0x6e,
};
static const uint8_t s_mp_challenge_y1[MP_CHALLENGE_LEN] = {
    0x18, 0x24, 0xd1, 0xac, 0x2a, 0x68, 0x07, 0x14, 0x8c, 0xe5, 0x1e, 0xef,
    0x60, 0xfd, 0x74, 0xac, 0xd7, 0x98, 0x68, 0xd4, 0x75, 0xf8, 0xeb, 0xcc,
    0x05, 0x86, 0x04, 0x05, 0x9e, 0xe8, 0x40, 0x76, 0x21, 0xf5, 0x5d, 0x65,
    0xc1, 0xf1, 0xed, 0xae, 0xcf, 0xa8, 0x24, 0x72, 0x0a, 0x15, 0x84, 0x7f,
    0xef, 0xe2, 0xc0, 0x04, 0xef, 0x4c, 0x68, 0x13, 0x68, 0x66, 0x48, 0x83,
    0x6f, 0xab, 0x61, 0x12,
};

static uint8_t s_regs[0x100];
static bool s_active;
static int64_t s_ready_at_us;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void reset_locked(void)
{
    memset(s_regs, 0, sizeof(s_regs));
    memcpy(&s_regs[MP_REG_CALIBRATION], s_mp_calibration, sizeof(s_mp_calibration));
    static const uint8_t inactive_id[6] = {0x00, 0x00, 0xa6, 0x20, 0x00, 0x05};
    memcpy(&s_regs[MP_REG_ID], inactive_id, sizeof(inactive_id));
    s_active = false;
    s_ready_at_us = 0;
}

void wiimote_motion_plus_reset(void)
{
    portENTER_CRITICAL(&s_mux);
    reset_locked();
    portEXIT_CRITICAL(&s_mux);
}

bool wiimote_motion_plus_is_active(void)
{
    portENTER_CRITICAL(&s_mux);
    bool active = s_active;
    portEXIT_CRITICAL(&s_mux);
    return active;
}

uint8_t wiimote_motion_plus_mode(void)
{
    portENTER_CRITICAL(&s_mux);
    const uint8_t mode = s_active ? s_regs[MP_REG_ID + 4] : 0;
    portEXIT_CRITICAL(&s_mux);
    return mode;
}

static void advance_challenge_locked(int64_t now)
{
    if (!s_active || s_ready_at_us == 0 || now < s_ready_at_us) return;
    s_ready_at_us = 0;
    if (s_regs[MP_REG_CHALLENGE_STATE] == MP_STATE_PREPARING_X) {
        memcpy(&s_regs[MP_REG_CHALLENGE], s_mp_challenge_x, MP_CHALLENGE_LEN);
        s_regs[MP_REG_CHALLENGE_STATE] = MP_STATE_X_READY;
    } else if (s_regs[MP_REG_CHALLENGE_STATE] == MP_STATE_PREPARING_Y) {
        memcpy(&s_regs[MP_REG_CHALLENGE],
               s_regs[MP_REG_CHALLENGE_TYPE] == 0 ? s_mp_challenge_y0 : s_mp_challenge_y1,
               MP_CHALLENGE_LEN);
        s_regs[MP_REG_CHALLENGE_STATE] = MP_STATE_Y_READY;
    }
}

static void activate_locked(uint8_t mode, int64_t now)
{
    s_active = true;
    s_regs[MP_REG_ID + 2] = 0xa4;
    s_regs[MP_REG_ID + 4] = mode;
    memset(&s_regs[MP_REG_DATA], 0, 0x20);
    memset(&s_regs[MP_REG_CHALLENGE], 0, MP_CHALLENGE_LEN);
    s_regs[MP_REG_CHALLENGE_STATE] = MP_STATE_PREPARING_X;
    s_ready_at_us = now + MP_PREPARE_X_US;
}

static void deactivate_locked(void)
{
    s_active = false;
    s_regs[MP_REG_ID + 2] = 0xa6;
    s_regs[MP_REG_ID + 4] = 0x00;
    s_regs[MP_REG_CHALLENGE_STATE] = 0;
    s_ready_at_us = 0;
}

static bool is_activation_mode(uint8_t mode)
{
    return mode == 0x04 || mode == 0x05 || mode == 0x07;
}

bool wiimote_motion_plus_read(uint8_t slave, uint16_t address, uint8_t length,
                              uint8_t *output, uint8_t *error)
{
#if NEXTMOTE_ENABLE_MOTION_PLUS
    if (slave != WIIMOTE_I2C_SLAVE_MOTION_PLUS && slave != WIIMOTE_I2C_SLAVE_EXTENSION)
        return false;

    portENTER_CRITICAL(&s_mux);
    const bool owns = s_active ? slave == WIIMOTE_I2C_SLAVE_EXTENSION :
                                 slave == WIIMOTE_I2C_SLAVE_MOTION_PLUS;
    if (!owns) {
        portEXIT_CRITICAL(&s_mux);
        /* Inactive: 0x52 keeps the "no extension" behaviour of the caller.
         * Active: the M+ stops answering at 0x53. */
        if (!s_active && slave == WIIMOTE_I2C_SLAVE_EXTENSION) return false;
        *error = WIIMOTE_MEMORY_ERROR_NACK;
        return true;
    }
    if ((uint32_t)address + length > sizeof(s_regs)) {
        portEXIT_CRITICAL(&s_mux);
        *error = WIIMOTE_MEMORY_ERROR_INVALID_ADDRESS;
        return true;
    }
    advance_challenge_locked(esp_timer_get_time());
    memcpy(output, &s_regs[address], length);
    portEXIT_CRITICAL(&s_mux);
    *error = WIIMOTE_MEMORY_ERROR_SUCCESS;
    return true;
#else
    (void)slave; (void)address; (void)length; (void)output; (void)error;
    return false;
#endif
}

bool wiimote_motion_plus_write(uint8_t slave, uint16_t address, const uint8_t *data,
                               uint8_t count, uint8_t *error, bool *status_changed)
{
#if NEXTMOTE_ENABLE_MOTION_PLUS
    *status_changed = false;
    if (slave != WIIMOTE_I2C_SLAVE_MOTION_PLUS && slave != WIIMOTE_I2C_SLAVE_EXTENSION)
        return false;

    portENTER_CRITICAL(&s_mux);
    const bool owns = s_active ? slave == WIIMOTE_I2C_SLAVE_EXTENSION :
                                 slave == WIIMOTE_I2C_SLAVE_MOTION_PLUS;
    if (!owns) {
        const bool was_active = s_active;
        portEXIT_CRITICAL(&s_mux);
        if (!was_active && slave == WIIMOTE_I2C_SLAVE_EXTENSION) return false;
        *error = WIIMOTE_MEMORY_ERROR_NACK;
        return true;
    }
    if ((uint32_t)address + count > sizeof(s_regs)) {
        portEXIT_CRITICAL(&s_mux);
        *error = WIIMOTE_MEMORY_ERROR_INVALID_ADDRESS;
        return true;
    }

    const int64_t now = esp_timer_get_time();
    advance_challenge_locked(now);
    memcpy(&s_regs[address], data, count);
    const uint16_t end = (uint16_t)(address + count);
    const char *event = NULL;

    if (!s_active) {
        if (address <= MP_REG_MODE && end > MP_REG_MODE) {
            const uint8_t mode = s_regs[MP_REG_MODE];
            if (is_activation_mode(mode)) {
                activate_locked(mode, now);
                /* WiiBrew: activation reports a plug event only when nothing is
                 * attached to the M+ passthrough port. */
                *status_changed = !wiimote_nunchuk_is_attached();
                event = "activated";
            }
        }
    } else if (address <= MP_REG_INIT && end > MP_REG_INIT) {
        deactivate_locked();
        *status_changed = true;
        event = "deactivated (0xf0)";
    } else if (address <= MP_REG_MODE && end > MP_REG_MODE &&
               !is_activation_mode(s_regs[MP_REG_MODE])) {
        deactivate_locked();
        *status_changed = true;
        event = "deactivated (mode)";
    } else if (address <= MP_REG_CHALLENGE_TYPE && end > MP_REG_CHALLENGE_TYPE &&
               s_regs[MP_REG_CHALLENGE_STATE] == MP_STATE_X_READY) {
        s_regs[MP_REG_CHALLENGE_STATE] = MP_STATE_PREPARING_Y;
        s_ready_at_us = now + (s_regs[MP_REG_CHALLENGE_TYPE] == 0 ?
                                   MP_PREPARE_Y0_US : MP_PREPARE_Y1_US);
        event = "challenge y requested";
    }
    const uint8_t mode = s_regs[MP_REG_ID + 4];
    portEXIT_CRITICAL(&s_mux);

    if (event != NULL) ESP_LOGI(TAG, "M+ %s (mode 0x%02x)", event, mode);
    *error = WIIMOTE_MEMORY_ERROR_SUCCESS;
    return true;
#else
    (void)slave; (void)address; (void)data; (void)count; (void)error;
    *status_changed = false;
    return false;
#endif
}

static uint16_t encode_axis(float rad_s, bool *slow)
{
    const float slow_limit = (MP_ZERO_VALUE - 1) / MP_SLOW_PER_RAD_S;
    *slow = fabsf(rad_s) < slow_limit;
    long value = lroundf(rad_s * (*slow ? MP_SLOW_PER_RAD_S : MP_FAST_PER_RAD_S)) +
                 MP_ZERO_VALUE;
    if (value < 0) value = 0;
    if (value > MP_MAX_VALUE) value = MP_MAX_VALUE;
    return (uint16_t)value;
}

void wiimote_motion_plus_encode(uint8_t output[WIIMOTE_MOTION_PLUS_DATA_LEN],
                                const float rad_s[3], bool valid)
{
    bool pitch_slow = true, roll_slow = true, yaw_slow = true;
    uint16_t pitch = MP_ZERO_VALUE, roll = MP_ZERO_VALUE, yaw = MP_ZERO_VALUE;
    if (valid && isfinite(rad_s[0]) && isfinite(rad_s[1]) && isfinite(rad_s[2])) {
        pitch = encode_axis(rad_s[0], &pitch_slow);
        roll = encode_axis(rad_s[1], &roll_slow);
        yaw = encode_axis(rad_s[2], &yaw_slow);
    }
    output[0] = (uint8_t)yaw;
    output[1] = (uint8_t)roll;
    output[2] = (uint8_t)pitch;
    output[3] = (uint8_t)(((yaw >> 8) << 2) | (yaw_slow ? 0x02U : 0U) | (pitch_slow ? 0x01U : 0U));
    /* Bit 0 (extension on the M+ passthrough port) is set by the caller. */
    output[4] = (uint8_t)(((roll >> 8) << 2) | (roll_slow ? 0x02U : 0U));
    /* Bit 1: this is M+ data (not passthrough extension data). */
    output[5] = (uint8_t)(((pitch >> 8) << 2) | 0x02U);
}

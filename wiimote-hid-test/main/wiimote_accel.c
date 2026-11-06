#include "wiimote_accel.h"

#include <math.h>
#include <stddef.h>

_Static_assert(WIIMOTE_ACCEL_X_AXIS >= 0 && WIIMOTE_ACCEL_X_AXIS < 3 &&
               WIIMOTE_ACCEL_Y_AXIS >= 0 && WIIMOTE_ACCEL_Y_AXIS < 3 &&
               WIIMOTE_ACCEL_Z_AXIS >= 0 && WIIMOTE_ACCEL_Z_AXIS < 3,
               "Mounting axes must be 0, 1, or 2");
_Static_assert(WIIMOTE_ACCEL_X_AXIS != WIIMOTE_ACCEL_Y_AXIS &&
               WIIMOTE_ACCEL_X_AXIS != WIIMOTE_ACCEL_Z_AXIS &&
               WIIMOTE_ACCEL_Y_AXIS != WIIMOTE_ACCEL_Z_AXIS,
               "Mounting must use each source axis once");
_Static_assert((WIIMOTE_ACCEL_X_SIGN == 1 || WIIMOTE_ACCEL_X_SIGN == -1) &&
               (WIIMOTE_ACCEL_Y_SIGN == 1 || WIIMOTE_ACCEL_Y_SIGN == -1) &&
               (WIIMOTE_ACCEL_Z_SIGN == 1 || WIIMOTE_ACCEL_Z_SIGN == -1),
               "Mounting signs must be +1 or -1");

bool wiimote_accel_encode(uint8_t payload[5], const float sensor_ms2[3])
{
    const unsigned axes[3] = {
        WIIMOTE_ACCEL_X_AXIS, WIIMOTE_ACCEL_Y_AXIS, WIIMOTE_ACCEL_Z_AXIS,
    };
    const float signs[3] = {
        WIIMOTE_ACCEL_X_SIGN, WIIMOTE_ACCEL_Y_SIGN, WIIMOTE_ACCEL_Z_SIGN,
    };
    if (payload == NULL || sensor_ms2 == NULL) return false;
    for (unsigned i = 0; i < 3; ++i) {
        if (!isfinite(sensor_ms2[i])) return false;
    }

    uint16_t raw[3];
    for (unsigned i = 0; i < 3; ++i) {
        float value = WIIMOTE_ACCEL_ZERO_BYTE * 4.0f +
                      sensor_ms2[axes[i]] * signs[i] *
                      ((WIIMOTE_ACCEL_ONE_G_BYTE - WIIMOTE_ACCEL_ZERO_BYTE) * 4.0f /
                       WIIMOTE_ACCEL_GRAVITY_MS2);
        value = fminf(1023.0f, fmaxf(0.0f, value));
        raw[i] = (uint16_t)lroundf(value);
        payload[2 + i] = (uint8_t)(raw[i] >> 2);
    }
    /* X bits 1:0 occupy button byte 0 bits 6:5; Y/Z bit 1 uses byte 1. */
    payload[0] = (uint8_t)((payload[0] & ~0x60U) | ((raw[0] & 3U) << 5));
    payload[1] = (uint8_t)((payload[1] & ~0x60U) |
                          ((raw[1] & 2U) << 4) | ((raw[2] & 2U) << 5));
    return true;
}

void wiimote_accel_encode_neutral(uint8_t payload[5])
{
    payload[0] &= (uint8_t)~0x60U;
    payload[1] &= (uint8_t)~0x60U;
    payload[2] = WIIMOTE_ACCEL_ZERO_BYTE;
    payload[3] = WIIMOTE_ACCEL_ZERO_BYTE;
    payload[4] = WIIMOTE_ACCEL_ONE_G_BYTE;
}

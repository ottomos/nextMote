#pragma once

#include <stdbool.h>
#include <stdint.h>

#define WIIMOTE_ACCEL_ZERO_BYTE 0x80U
#define WIIMOTE_ACCEL_ONE_G_BYTE 0x9aU
#define WIIMOTE_ACCEL_GRAVITY_MS2 9.80665f

/* Mounting: source axes 0=X, 1=Y, 2=Z; signs must be +1 or -1. */
#ifndef WIIMOTE_ACCEL_X_AXIS
#define WIIMOTE_ACCEL_X_AXIS 0
#endif
#ifndef WIIMOTE_ACCEL_Y_AXIS
#define WIIMOTE_ACCEL_Y_AXIS 1
#endif
#ifndef WIIMOTE_ACCEL_Z_AXIS
#define WIIMOTE_ACCEL_Z_AXIS 2
#endif
#ifndef WIIMOTE_ACCEL_X_SIGN
#define WIIMOTE_ACCEL_X_SIGN 1
#endif
#ifndef WIIMOTE_ACCEL_Y_SIGN
#define WIIMOTE_ACCEL_Y_SIGN 1
#endif
#ifndef WIIMOTE_ACCEL_Z_SIGN
#define WIIMOTE_ACCEL_Z_SIGN 1
#endif

bool wiimote_accel_encode(uint8_t payload[5], const float sensor_ms2[3]);
void wiimote_accel_encode_neutral(uint8_t payload[5]);

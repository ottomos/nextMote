#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 1 exposes an emulated Wii MotionPlus (gyro from the BNO08x) on the
 * extension port; 0 keeps the plain Wii Remote (no extension). */
#ifndef NEXTMOTE_ENABLE_MOTION_PLUS
#define NEXTMOTE_ENABLE_MOTION_PLUS 0
#endif

#define WIIMOTE_I2C_SLAVE_MOTION_PLUS 0x53
#define WIIMOTE_MOTION_PLUS_DATA_LEN 6

void wiimote_motion_plus_reset(void);
bool wiimote_motion_plus_is_active(void);

/* Return true when the access was handled by the M+; *error is then valid. */
bool wiimote_motion_plus_read(uint8_t slave, uint16_t address, uint8_t length,
                              uint8_t *output, uint8_t *error);
bool wiimote_motion_plus_write(uint8_t slave, uint16_t address, const uint8_t *data,
                               uint8_t count, uint8_t *error, bool *status_changed);

/* Gyro in rad/s about the Wii Remote X (pitch), Y (roll), Z (yaw) axes. */
void wiimote_motion_plus_encode(uint8_t output[WIIMOTE_MOTION_PLUS_DATA_LEN],
                                const float rad_s[3], bool valid);

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define WIIMOTE_NUNCHUK_DATA_LEN 6
#define WIIMOTE_NUNCHUK_STICK_CENTER 0x80
#define WIIMOTE_NUNCHUK_ACCEL_ZERO 0x200
#define WIIMOTE_NUNCHUK_ACCEL_ONE_G 0x2cc

typedef struct {
    uint8_t stick_x;
    uint8_t stick_y;
    uint16_t accel[3]; /* 10-bit X, Y, Z */
    bool c;
    bool z;
} wiimote_nunchuk_input_t;

typedef struct {
    bool attached;
    bool encrypted;
    bool unsupported_key;
    uint32_t encryption_enables;
    wiimote_nunchuk_input_t input;
} wiimote_nunchuk_status_t;

/* Clears host-written registers (encryption, key) on a new HID connection. */
void wiimote_nunchuk_reset(void);
/* Returns true when the attach state changed. */
bool wiimote_nunchuk_set_attached(bool attached);
bool wiimote_nunchuk_is_attached(void);
void wiimote_nunchuk_set_input(const wiimote_nunchuk_input_t *input);
void wiimote_nunchuk_neutral_input(wiimote_nunchuk_input_t *input);
void wiimote_nunchuk_get_status(wiimote_nunchuk_status_t *status);

/* Return true when the access was handled by the Nunchuk; *error is then valid. */
bool wiimote_nunchuk_read(uint8_t slave, uint16_t address, uint8_t length,
                          uint8_t *output, uint8_t *error);
bool wiimote_nunchuk_write(uint8_t slave, uint16_t address, const uint8_t *data,
                           uint8_t count, uint8_t *error);

/* Report bytes as streamed from register 0x00 (encrypted when the host enabled it). */
void wiimote_nunchuk_encode(uint8_t *output, uint8_t length);

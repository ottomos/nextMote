#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
binary=$(mktemp "${TMPDIR:-/tmp}/nextmote-imu-hid.XXXXXX")
trap 'rm -f -- "$binary"' EXIT

build_and_run() {
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -I"$root/test/host" -I"$root/nextmote-wii/main" \
        "$root/test/test_imu_hid.c" \
        "$root/nextmote-wii/main/wiimote_accel.c" \
        "$root/nextmote-wii/main/wiimote_reports.c" \
        "$root/nextmote-wii/main/wiimote_state.c" \
        "$root/nextmote-wii/main/wiimote_protocol.c" \
        "$root/nextmote-wii/main/wiimote_motion_plus.c" \
        "$root/nextmote-wii/main/wiimote_nunchuk.c" \
        -DNEXTMOTE_SEND_IMU_TO_HID=1 -DNEXTMOTE_ENABLE_MOTION_PLUS=1 "$@" -lm -o "$binary"
    "$binary"

}

build_and_run
build_and_run -DWIIMOTE_ACCEL_X_AXIS=1 -DWIIMOTE_ACCEL_Y_AXIS=0 \
    -DWIIMOTE_ACCEL_Z_AXIS=2 -DWIIMOTE_ACCEL_X_SIGN=-1 \
    -DWIIMOTE_ACCEL_Y_SIGN=1 -DWIIMOTE_ACCEL_Z_SIGN=1

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I"$root/nextmote-wii/main" \
    "$root/test/test_nextmote_form.c" "$root/nextmote-wii/main/nextmote_form.c" -o "$binary"
"$binary"

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
    -I"$root/test/host" -I"$root/nextmote-wii/main" \
    "$root/test/test_physical_buttons.c" \
    "$root/nextmote-wii/main/wiimote_button_input.c" \
    "$root/nextmote-wii/main/wiimote_state.c" \
    "$root/nextmote-wii/main/wiimote_protocol.c" -o "$binary"
"$binary"

#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
binary=$(mktemp "${TMPDIR:-/tmp}/nextmote-imu-hid.XXXXXX")
trap 'rm -f -- "$binary"' EXIT

build_and_run() {
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -I"$root/test/host" -I"$root/wiimote-hid-test/main" \
        "$root/test/test_imu_hid.c" \
        "$root/wiimote-hid-test/main/wiimote_accel.c" \
        "$root/wiimote-hid-test/main/wiimote_reports.c" \
        "$root/wiimote-hid-test/main/wiimote_state.c" \
        "$root/wiimote-hid-test/main/wiimote_protocol.c" \
        "$root/wiimote-hid-test/main/wiimote_motion_plus.c" \
        "$root/wiimote-hid-test/main/wiimote_nunchuk.c" \
        -DNEXTMOTE_SEND_IMU_TO_HID=1 -DNEXTMOTE_ENABLE_MOTION_PLUS=1 "$@" -lm -o "$binary"
    "$binary"
}

build_and_run
build_and_run -DWIIMOTE_ACCEL_X_AXIS=1 -DWIIMOTE_ACCEL_Y_AXIS=0 \
    -DWIIMOTE_ACCEL_Z_AXIS=2 -DWIIMOTE_ACCEL_X_SIGN=-1 \
    -DWIIMOTE_ACCEL_Y_SIGN=1 -DWIIMOTE_ACCEL_Z_SIGN=1

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I"$root/wiimote-hid-test/main" \
    "$root/test/test_nextmote_form.c" "$root/wiimote-hid-test/main/nextmote_form.c" -o "$binary"
"$binary"

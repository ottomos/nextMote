#include <assert.h>
#include <stdio.h>

#include "nunchuk_adapter_protocol.h"

static void test_plain_sample_decode(void)
{
    const uint8_t raw[6] = {128, 17, 0x12, 0x23, 0x34, 0x69};
    wiimote_nunchuk_input_t input = {0};
    nunchuk_adapter_decode(raw, &input);

    assert(input.stick_x == 128);
    assert(input.stick_y == 17);
    assert(input.accel[0] == ((0x12U << 2) | 1U));
    assert(input.accel[1] == ((0x23U << 2) | 2U));
    assert(input.accel[2] == ((0x34U << 2) | 2U));
    assert(input.c);
    assert(!input.z);
}

static void test_buttons_and_acceleration_extremes(void)
{
    const uint8_t pressed[6] = {0, 255, 0, 255, 0, 0x00};
    wiimote_nunchuk_input_t input = {0};
    nunchuk_adapter_decode(pressed, &input);
    assert(input.stick_x == 0);
    assert(input.stick_y == 255);
    assert(input.accel[0] == 0);
    assert(input.accel[1] == 1020);
    assert(input.accel[2] == 0);
    assert(input.c && input.z);

    const uint8_t released[6] = {0, 0, 255, 0, 255, 0xcf};
    nunchuk_adapter_decode(released, &input);
    assert(input.accel[0] == 1023);
    assert(input.accel[1] == 0);
    assert(input.accel[2] == 1023);
    assert(!input.c && !input.z);
}

static void test_null_arguments(void)
{
    const uint8_t raw[6] = {0};
    wiimote_nunchuk_input_t input = {0};
    nunchuk_adapter_decode(NULL, &input);
    nunchuk_adapter_decode(raw, NULL);
}

int main(void)
{
    test_plain_sample_decode();
    test_buttons_and_acceleration_extremes();
    test_null_arguments();
    puts("Nunchuk adapter decode: stick, 10-bit acceleration and active-low C/Z PASS");
}

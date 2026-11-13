#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "wiimote_button_input.h"
#include "wiimote_state.h"

static uint16_t current_mask(void)
{
    uint8_t buttons[2];
    wiimote_state_get_buttons(buttons);
    return ((uint16_t)buttons[0] << 8) | buttons[1];
}

static void test_debounce(void)
{
    wiimote_button_debounce_t button = {0};
    assert(!wiimote_button_debounce_update(&button, true, 100));
    assert(!wiimote_button_debounce_update(&button, false, 110));
    assert(!wiimote_button_debounce_update(&button, true, 115));
    assert(!wiimote_button_debounce_update(&button, true, 144));
    assert(wiimote_button_debounce_update(&button, true, 145));
    assert(button.pressed);
    assert(!wiimote_button_debounce_update(&button, true, 200));
    assert(!wiimote_button_debounce_update(&button, false, 210));
    assert(!wiimote_button_debounce_update(&button, true, 220));
    assert(!wiimote_button_debounce_update(&button, false, 230));
    assert(!wiimote_button_debounce_update(&button, false, 259));
    assert(wiimote_button_debounce_update(&button, false, 260));
    assert(!button.pressed);

    button = (wiimote_button_debounce_t){0};
    assert(!wiimote_button_debounce_update(&button, true, UINT32_MAX - 10));
    assert(!wiimote_button_debounce_update(&button, true, 18));
    assert(wiimote_button_debounce_update(&button, true, 19));
}

static void test_mapping(void)
{
    const uint16_t masks[] = {0x0800, 0x0200, 0x0400, 0x0100, 0x0008, 0x0004};
    for (unsigned bits = 0; bits < 64; ++bits) {
        uint16_t expected = 0;
        for (unsigned i = 0; i < 6; ++i) {
            if (bits & (1U << i)) expected |= masks[i];
        }
        assert(wiimote_physical_button_mask((uint8_t)bits) == expected);
    }
    assert(wiimote_physical_button_mask(0xff) == 0x0f0c);
}

static void test_sources(void)
{
    wiimote_state_open();
    assert(wiimote_state_set_button_mask(0x0008));
    assert(wiimote_state_set_physical_button_mask(0x0804));
    assert(current_mask() == 0x080c);
    assert(wiimote_state_set_button_mask(0));
    assert(current_mask() == 0x0804);
    assert(wiimote_state_set_button_mask(0x0004));
    assert(wiimote_state_set_physical_button_mask(0));
    assert(current_mask() == 0x0004);

    assert(wiimote_state_set_physical_button_mask(0x0004));
    assert(wiimote_state_set_button_mask(0));
    assert(current_mask() == 0x0004);
    wiimote_state_close();
    assert(!wiimote_state_set_physical_button_mask(0x0200));
    wiimote_state_open();
    assert(current_mask() == 0x0200);
    wiimote_state_set_button_bytes(0x10, 0x00);
    assert(current_mask() == 0x1200);
    wiimote_state_set_physical_button_mask(0);
    wiimote_state_close();
    assert(current_mask() == 0);
}

int main(void)
{
    test_debounce();
    test_mapping();
    test_sources();
    puts("Physical buttons: debounce, wraparound, 64 combinations, source merging and reconnect PASS");
}

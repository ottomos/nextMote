#include "wiimote_button_input.h"

bool wiimote_button_debounce_update(wiimote_button_debounce_t *button,
                                    bool pressed, uint32_t now_ms)
{
    if (pressed != button->candidate) {
        button->candidate = pressed;
        button->candidate_since_ms = now_ms;
    }
    if (button->pressed == button->candidate ||
        (uint32_t)(now_ms - button->candidate_since_ms) < WIIMOTE_BUTTON_DEBOUNCE_MS) {
        return false;
    }
    button->pressed = button->candidate;
    return true;
}

uint16_t wiimote_physical_button_mask(uint8_t pressed_bits)
{
    static const uint16_t masks[WIIMOTE_PHYSICAL_BUTTON_COUNT] = {
        0x0800, 0x0200, 0x0400, 0x0100, 0x0008, 0x0004,
    };
    uint16_t mask = 0;
    for (unsigned i = 0; i < WIIMOTE_PHYSICAL_BUTTON_COUNT; ++i) {
        if (pressed_bits & (1U << i)) mask |= masks[i];
    }
    return mask;
}

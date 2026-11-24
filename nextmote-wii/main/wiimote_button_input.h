#pragma once

#include <stdbool.h>
#include <stdint.h>

#define WIIMOTE_PHYSICAL_BUTTON_COUNT 6
#define WIIMOTE_BUTTON_DEBOUNCE_MS 30

typedef struct {
    bool candidate;
    bool pressed;
    uint32_t candidate_since_ms;
} wiimote_button_debounce_t;

bool wiimote_button_debounce_update(wiimote_button_debounce_t *button,
                                    bool pressed, uint32_t now_ms);
uint16_t wiimote_physical_button_mask(uint8_t pressed_bits);

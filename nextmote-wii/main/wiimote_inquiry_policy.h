#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t wiimote_iac_t[3];

bool wiimote_inquiry_parse_capacity(const uint8_t *event, size_t length,
                                    uint8_t *capacity);
bool wiimote_inquiry_select_codes(uint8_t capacity, uint8_t count,
                                  const wiimote_iac_t *requested,
                                  wiimote_iac_t *selected, uint8_t *selected_count);
void wiimote_inquiry_get_code(bool general, wiimote_iac_t code);

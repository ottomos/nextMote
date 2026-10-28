#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef void (*wiimote_web_button_callback_t)(uint16_t state);

void wiimote_web_start(void);
void wiimote_web_set_hid_connected(bool connected);
void wiimote_web_set_button_callback(wiimote_web_button_callback_t callback);

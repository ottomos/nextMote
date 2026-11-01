#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "ir_camera.h"

#define WIIMOTE_SYNC_WINDOW_SECONDS 30U

typedef void (*wiimote_web_button_callback_t)(uint16_t state);
typedef esp_err_t (*wiimote_web_sync_callback_t)(bool general_discoverable);

void wiimote_web_start(void);
void wiimote_web_set_hid_connected(bool connected);
void wiimote_web_set_sync_callback(wiimote_web_sync_callback_t callback);
void wiimote_web_set_sync_window(bool active);
void wiimote_web_set_button_callback(wiimote_web_button_callback_t callback);
void wiimote_web_set_camera_state(const ir_camera_state_t *state);

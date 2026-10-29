#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "wiimote_protocol.h"

typedef struct {
    uint8_t buttons[WIIMOTE_BUTTON_PAYLOAD_LEN];
    uint8_t leds;
    bool ir_logic_enabled;
    bool ir_sensor_enabled;
    uint8_t report_mode;
    bool continuous_reporting;
    bool connected;
} wiimote_state_snapshot_t;

void wiimote_state_open(void);
void wiimote_state_close(void);
bool wiimote_state_is_connected(void);
bool wiimote_state_ir_enabled(void);
bool wiimote_state_set_button_mask(uint16_t button_mask);
void wiimote_state_set_button_bytes(uint8_t first, uint8_t second);
void wiimote_state_get_buttons(uint8_t buttons[WIIMOTE_BUTTON_PAYLOAD_LEN]);
void wiimote_state_set_leds(uint8_t leds);
void wiimote_state_set_ir_feature(uint8_t report_id, bool enabled);
void wiimote_state_set_report_mode(uint8_t report_mode, bool continuous_reporting);
void wiimote_state_get_report_mode(uint8_t *report_mode, bool *continuous_reporting);
void wiimote_state_get_snapshot(wiimote_state_snapshot_t *snapshot);

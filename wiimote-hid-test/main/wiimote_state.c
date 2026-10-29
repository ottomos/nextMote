#include "wiimote_state.h"

#include <stddef.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static wiimote_state_snapshot_t s_state;
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;

void wiimote_state_open(void)
{
    portENTER_CRITICAL(&s_state_mux);
    s_state.connected = true;
    memset(s_state.buttons, 0, sizeof(s_state.buttons));
    s_state.ir_logic_enabled = false;
    s_state.ir_sensor_enabled = false;
    portEXIT_CRITICAL(&s_state_mux);
}

void wiimote_state_close(void)
{
    portENTER_CRITICAL(&s_state_mux);
    s_state.connected = false;
    memset(s_state.buttons, 0, sizeof(s_state.buttons));
    s_state.ir_logic_enabled = false;
    s_state.ir_sensor_enabled = false;
    portEXIT_CRITICAL(&s_state_mux);
}

bool wiimote_state_is_connected(void)
{
    portENTER_CRITICAL(&s_state_mux);
    bool connected = s_state.connected;
    portEXIT_CRITICAL(&s_state_mux);
    return connected;
}

bool wiimote_state_ir_enabled(void)
{
    portENTER_CRITICAL(&s_state_mux);
    bool enabled = s_state.ir_logic_enabled && s_state.ir_sensor_enabled;
    portEXIT_CRITICAL(&s_state_mux);
    return enabled;
}

bool wiimote_state_set_button_mask(uint16_t button_mask)
{
    portENTER_CRITICAL(&s_state_mux);
    s_state.buttons[0] = (uint8_t)(button_mask >> 8);
    s_state.buttons[1] = (uint8_t)button_mask;
    bool send_now = s_state.connected &&
                    wiimote_protocol_is_data_report(s_state.report_mode);
    portEXIT_CRITICAL(&s_state_mux);
    return send_now;
}

void wiimote_state_set_button_bytes(uint8_t first, uint8_t second)
{
    portENTER_CRITICAL(&s_state_mux);
    s_state.buttons[0] = first;
    s_state.buttons[1] = second;
    portEXIT_CRITICAL(&s_state_mux);
}

void wiimote_state_get_buttons(uint8_t buttons[WIIMOTE_BUTTON_PAYLOAD_LEN])
{
    if (buttons == NULL) return;
    portENTER_CRITICAL(&s_state_mux);
    memcpy(buttons, s_state.buttons, sizeof(s_state.buttons));
    portEXIT_CRITICAL(&s_state_mux);
}

void wiimote_state_set_leds(uint8_t leds)
{
    portENTER_CRITICAL(&s_state_mux);
    s_state.leds = leds & 0xf0U;
    portEXIT_CRITICAL(&s_state_mux);
}

void wiimote_state_set_ir_feature(uint8_t report_id, bool enabled)
{
    portENTER_CRITICAL(&s_state_mux);
    if (report_id == WIIMOTE_REPORT_IR_ENABLE) {
        s_state.ir_logic_enabled = enabled;
    } else if (report_id == WIIMOTE_REPORT_IR_ENABLE_2) {
        s_state.ir_sensor_enabled = enabled;
    }
    portEXIT_CRITICAL(&s_state_mux);
}

void wiimote_state_set_report_mode(uint8_t report_mode, bool continuous_reporting)
{
    portENTER_CRITICAL(&s_state_mux);
    s_state.report_mode = report_mode;
    s_state.continuous_reporting = continuous_reporting;
    portEXIT_CRITICAL(&s_state_mux);
}

void wiimote_state_get_report_mode(uint8_t *report_mode, bool *continuous_reporting)
{
    portENTER_CRITICAL(&s_state_mux);
    if (report_mode != NULL) *report_mode = s_state.report_mode;
    if (continuous_reporting != NULL) *continuous_reporting = s_state.continuous_reporting;
    portEXIT_CRITICAL(&s_state_mux);
}

void wiimote_state_get_snapshot(wiimote_state_snapshot_t *snapshot)
{
    if (snapshot == NULL) return;
    portENTER_CRITICAL(&s_state_mux);
    *snapshot = s_state;
    portEXIT_CRITICAL(&s_state_mux);
}

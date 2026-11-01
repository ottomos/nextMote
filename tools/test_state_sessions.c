#include <assert.h>
#include <stdio.h>

#include "wiimote_state.h"

int main(void)
{
    wiimote_state_open();
    wiimote_state_set_report_mode(WIIMOTE_REPORT_BUTTONS_ACCEL_IR, true);
    wiimote_state_set_button_mask(0x0008);
    wiimote_state_set_ir_feature(WIIMOTE_REPORT_IR_ENABLE, true);
    wiimote_state_set_ir_feature(WIIMOTE_REPORT_IR_ENABLE_2, true);
    wiimote_state_close();
    wiimote_state_open();

    wiimote_state_snapshot_t state;
    wiimote_state_get_snapshot(&state);
    assert(state.connected);
    assert(state.buttons[0] == 0 && state.buttons[1] == 0);
    assert(!state.ir_logic_enabled && !state.ir_sensor_enabled);
    assert(state.report_mode == WIIMOTE_REPORT_BUTTONS);
    assert(!state.continuous_reporting);
    puts("New HID session resets to core report 0x30 without prior-host reporting state: PASS");
    return 0;
}

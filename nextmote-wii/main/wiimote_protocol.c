#include "wiimote_protocol.h"

#include <stddef.h>

bool wiimote_protocol_is_data_report(uint8_t report_id)
{
    return report_id == WIIMOTE_REPORT_BUTTONS ||
           wiimote_protocol_report_has_sensor_data(report_id);
}

/* Data reports that carry accelerometer, IR, or extension bytes. */
bool wiimote_protocol_report_has_sensor_data(uint8_t report_id)
{
    switch (report_id) {
    case WIIMOTE_REPORT_BUTTONS_ACCEL:
    case WIIMOTE_REPORT_BUTTONS_EXT8:
    case WIIMOTE_REPORT_BUTTONS_ACCEL_IR:
    case WIIMOTE_REPORT_BUTTONS_EXT19:
    case WIIMOTE_REPORT_BUTTONS_ACCEL_EXT16:
    case WIIMOTE_REPORT_BUTTONS_IR10_EXT9:
    case WIIMOTE_REPORT_BUTTONS_ACCEL_IR10_EXT6:
    case WIIMOTE_REPORT_EXT21:
        return true;
    default:
        return false;
    }
}

bool wiimote_protocol_report_requests_ack(const uint8_t *payload, uint16_t length)
{
    return payload != NULL && length > 0 && (payload[0] & 0x02U) != 0;
}

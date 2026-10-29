#include "wiimote_protocol.h"

#include <stddef.h>

bool wiimote_protocol_is_data_report(uint8_t report_id)
{
    return report_id == WIIMOTE_REPORT_BUTTONS ||
           report_id == WIIMOTE_REPORT_BUTTONS_ACCEL ||
           report_id == WIIMOTE_REPORT_BUTTONS_ACCEL_IR;
}

bool wiimote_protocol_report_requests_ack(const uint8_t *payload, uint16_t length)
{
    return payload != NULL && length > 0 && (payload[0] & 0x02U) != 0;
}

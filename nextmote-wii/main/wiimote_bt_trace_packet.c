#include "wiimote_bt_trace_packet.h"

#include <stdbool.h>

static bool is_discovery_command(uint16_t opcode)
{
    switch (opcode) {
    case 0x0405: /* Create Connection */
    case 0x0406: /* Disconnect */
    case 0x0409: /* Accept Connection Request */
    case 0x040a: /* Reject Connection Request */
    case 0x0411: /* Authentication Requested */
    case 0x0c19: /* Read Scan Enable */
    case 0x0c1a: /* Write Scan Enable */
    case 0x0c1b: /* Read Page Scan Activity */
    case 0x0c1c: /* Write Page Scan Activity */
    case 0x0c1d: /* Read Inquiry Scan Activity */
    case 0x0c1e: /* Write Inquiry Scan Activity */
    case 0x0c23: /* Read Class of Device */
    case 0x0c24: /* Write Class of Device */
    case 0x0c38: /* Read Number of Supported IAC */
    case 0x0c39: /* Read Current IAC LAP */
    case 0x0c3a: /* Write Current IAC LAP */
    case 0x0c42: /* Read Inquiry Scan Type */
    case 0x0c43: /* Write Inquiry Scan Type */
    case 0x0c46: /* Read Page Scan Type */
    case 0x0c47: /* Write Page Scan Type */
        return true;
    default:
        return false;
    }
}

static uint16_t read_le16(const uint8_t *bytes)
{
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

wiimote_bt_trace_filter_result_t wiimote_bt_trace_filter_packet(
    const uint8_t *packet, size_t length)
{
    if (!packet || length == 0) return WIIMOTE_BT_TRACE_MALFORMED;

    switch (packet[0]) {
    case 0x01:
        if (length < 4) return WIIMOTE_BT_TRACE_MALFORMED;
        if (!is_discovery_command(read_le16(&packet[1]))) {
            return WIIMOTE_BT_TRACE_IGNORE;
        }
        return length == (size_t)packet[3] + 4
                   ? WIIMOTE_BT_TRACE_CAPTURE : WIIMOTE_BT_TRACE_MALFORMED;
    case 0x04:
        if (length < 3 || length != (size_t)packet[2] + 3) {
            return WIIMOTE_BT_TRACE_MALFORMED;
        }
        switch (packet[1]) {
        case 0x0e: /* Command Complete */
        case 0x0f: /* Command Status */
            if (length < 7) return WIIMOTE_BT_TRACE_MALFORMED;
            return is_discovery_command(read_le16(&packet[4 + (packet[1] == 0x0f)]))
                       ? WIIMOTE_BT_TRACE_CAPTURE : WIIMOTE_BT_TRACE_IGNORE;
        case 0x03: /* Connection Complete */
            return packet[2] == 11
                       ? WIIMOTE_BT_TRACE_CAPTURE : WIIMOTE_BT_TRACE_MALFORMED;
        case 0x04: /* Connection Request */
            return packet[2] == 10
                       ? WIIMOTE_BT_TRACE_CAPTURE : WIIMOTE_BT_TRACE_MALFORMED;
        case 0x05: /* Disconnection Complete */
            return packet[2] == 4
                       ? WIIMOTE_BT_TRACE_CAPTURE : WIIMOTE_BT_TRACE_MALFORMED;
        case 0x06: /* Authentication Complete */
            return packet[2] == 3
                       ? WIIMOTE_BT_TRACE_CAPTURE : WIIMOTE_BT_TRACE_MALFORMED;
        case 0x16: /* PIN Code Request: address only, not the PIN reply */
        case 0x17: /* Link Key Request: address only, not the key */
            return packet[2] == 6
                       ? WIIMOTE_BT_TRACE_CAPTURE : WIIMOTE_BT_TRACE_MALFORMED;
        default:
            return WIIMOTE_BT_TRACE_IGNORE;
        }
    default:
        return WIIMOTE_BT_TRACE_IGNORE;
    }
}

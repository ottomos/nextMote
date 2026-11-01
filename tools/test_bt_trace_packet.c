#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wiimote_bt_trace_packet.h"

static void check(const uint8_t *packet, size_t length,
                  wiimote_bt_trace_filter_result_t expected)
{
    uint8_t before[64];
    assert(length <= sizeof(before));
    if (packet) memcpy(before, packet, length);
    assert(wiimote_bt_trace_filter_packet(packet, length) == expected);
    if (packet) assert(memcmp(before, packet, length) == 0);
}

int main(void)
{
    const uint8_t cod_write[] = {0x01, 0x24, 0x0c, 0x03, 0x04, 0x25, 0x00};
    const uint8_t cod_complete[] = {0x04, 0x0e, 0x04, 0x01, 0x24, 0x0c, 0x00};
    const uint8_t cod_read[] = {0x01, 0x23, 0x0c, 0x00};
    const uint8_t cod_read_complete[] = {
        0x04, 0x0e, 0x07, 0x01, 0x23, 0x0c, 0x00, 0x04, 0x25, 0x00,
    };
    const uint8_t iac_read_complete[] = {
        0x04, 0x0e, 0x0b, 0x01, 0x39, 0x0c, 0x00, 0x02,
        0x00, 0x8b, 0x9e, 0x33, 0x8b, 0x9e,
    };
    const uint8_t iac_write[] = {
        0x01, 0x3a, 0x0c, 0x07, 0x02, 0x00, 0x8b, 0x9e, 0x33, 0x8b, 0x9e,
    };
    const uint8_t command_failure[] = {0x04, 0x0f, 0x04, 0x12, 0x01, 0x3a, 0x0c};
    const uint8_t connection_request[] = {
        0x04, 0x04, 0x0a, 1, 2, 3, 4, 5, 6, 0x04, 0x25, 0x00, 0x01,
    };
    const uint8_t connection_complete[] = {
        0x04, 0x03, 0x0b, 0x00, 0x01, 0x00, 1, 2, 3, 4, 5, 6, 0x01, 0x00,
    };
    const uint8_t disconnection[] = {0x04, 0x05, 0x04, 0x00, 0x01, 0x00, 0x13};
    const uint8_t authentication[] = {0x04, 0x06, 0x03, 0x00, 0x01, 0x00};
    const uint8_t pin_request[] = {0x04, 0x16, 0x06, 1, 2, 3, 4, 5, 6};
    const uint8_t key_request[] = {0x04, 0x17, 0x06, 1, 2, 3, 4, 5, 6};
    const uint8_t pin_reply[27] = {0x01, 0x0d, 0x04, 0x17};
    const uint8_t key_reply[26] = {0x01, 0x0b, 0x04, 0x16};
    const uint8_t key_notification[26] = {0x04, 0x18, 0x17};
    const uint8_t acl_payload[] = {0x02, 0x01, 0x00, 0x01, 0x00, 0xaa};
    const uint8_t unrelated_complete[] = {0x04, 0x0e, 0x04, 0x01, 0x0d, 0x04, 0x00};
    const uint8_t unrelated_event[] = {0x04, 0x13, 0x01, 0x00};
    const uint8_t short_complete[] = {0x04, 0x0e, 0x02, 0x01, 0x24};
    const uint8_t invalid_connection[] = {0x04, 0x04, 0x01, 0x00};

    check(cod_write, sizeof(cod_write), WIIMOTE_BT_TRACE_CAPTURE);
    check(cod_complete, sizeof(cod_complete), WIIMOTE_BT_TRACE_CAPTURE);
    check(cod_read, sizeof(cod_read), WIIMOTE_BT_TRACE_CAPTURE);
    check(cod_read_complete, sizeof(cod_read_complete), WIIMOTE_BT_TRACE_CAPTURE);
    check(iac_read_complete, sizeof(iac_read_complete), WIIMOTE_BT_TRACE_CAPTURE);
    check(iac_write, sizeof(iac_write), WIIMOTE_BT_TRACE_CAPTURE);
    check(command_failure, sizeof(command_failure), WIIMOTE_BT_TRACE_CAPTURE);
    check(connection_request, sizeof(connection_request), WIIMOTE_BT_TRACE_CAPTURE);
    check(connection_complete, sizeof(connection_complete), WIIMOTE_BT_TRACE_CAPTURE);
    check(disconnection, sizeof(disconnection), WIIMOTE_BT_TRACE_CAPTURE);
    check(authentication, sizeof(authentication), WIIMOTE_BT_TRACE_CAPTURE);
    check(pin_request, sizeof(pin_request), WIIMOTE_BT_TRACE_CAPTURE);
    check(key_request, sizeof(key_request), WIIMOTE_BT_TRACE_CAPTURE);
    check(pin_reply, sizeof(pin_reply), WIIMOTE_BT_TRACE_IGNORE);
    check(key_reply, sizeof(key_reply), WIIMOTE_BT_TRACE_IGNORE);
    check(key_notification, sizeof(key_notification), WIIMOTE_BT_TRACE_IGNORE);
    check(acl_payload, sizeof(acl_payload), WIIMOTE_BT_TRACE_IGNORE);
    check(unrelated_complete, sizeof(unrelated_complete), WIIMOTE_BT_TRACE_IGNORE);
    check(unrelated_event, sizeof(unrelated_event), WIIMOTE_BT_TRACE_IGNORE);
    check(short_complete, sizeof(short_complete), WIIMOTE_BT_TRACE_MALFORMED);
    check(invalid_connection, sizeof(invalid_connection), WIIMOTE_BT_TRACE_MALFORMED);
    check(NULL, 0, WIIMOTE_BT_TRACE_MALFORMED);
    for (size_t length = 0; length < sizeof(cod_write); ++length) {
        check(cod_write, length, WIIMOTE_BT_TRACE_MALFORMED);
    }
    for (size_t length = 0; length < sizeof(cod_complete); ++length) {
        check(cod_complete, length, WIIMOTE_BT_TRACE_MALFORMED);
    }
    puts("HCI trace packet selection, truncation guards, credential exclusion, and immutability: PASS");
    return 0;
}

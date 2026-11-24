#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum {
    WIIMOTE_BT_TRACE_IGNORE,
    WIIMOTE_BT_TRACE_CAPTURE,
    WIIMOTE_BT_TRACE_MALFORMED,
} wiimote_bt_trace_filter_result_t;

wiimote_bt_trace_filter_result_t wiimote_bt_trace_filter_packet(
    const uint8_t *packet, size_t length);

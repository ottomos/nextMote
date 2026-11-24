#pragma once

#include <stddef.h>
#include <stdint.h>

#define WIIMOTE_DEVICE_ID_VERSION 0x8500U

typedef enum {
    WIIMOTE_SDP_UINT8,
    WIIMOTE_SDP_UINT16,
    WIIMOTE_SDP_BOOLEAN,
} wiimote_sdp_value_type_t;

typedef struct {
    uint16_t id;
    wiimote_sdp_value_type_t type;
    uint16_t value;
} wiimote_sdp_value_t;

typedef struct {
    const wiimote_sdp_value_t *values;
    size_t count;
} wiimote_sdp_values_t;

wiimote_sdp_values_t wiimote_sdp_hid_values(void);
wiimote_sdp_values_t wiimote_sdp_device_id_values(void);

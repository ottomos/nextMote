#include "wiimote_sdp_values.h"

/* Measured from the reference RVL-CNT-01, not generic HID defaults. */
static const wiimote_sdp_value_t s_hid_values[] = {
    {0x0202, WIIMOTE_SDP_UINT8, 0x04},       /* Subclass */
    {0x0203, WIIMOTE_SDP_UINT8, 0x33},       /* Country code */
    {0x0204, WIIMOTE_SDP_BOOLEAN, 0},       /* Virtual cable */
    {0x0208, WIIMOTE_SDP_BOOLEAN, 0},       /* SDP disable */
    {0x020a, WIIMOTE_SDP_BOOLEAN, 1},       /* Remote wake */
    {0x020c, WIIMOTE_SDP_UINT16, 0x0c80},   /* Link supervision timeout */
    {0x020d, WIIMOTE_SDP_BOOLEAN, 0},       /* Normally connectable */
    {0x020e, WIIMOTE_SDP_BOOLEAN, 0},       /* Boot device */
};

static const wiimote_sdp_value_t s_device_id_values[] = {
    {0x0200, WIIMOTE_SDP_UINT16, 0x0100},
    {0x0203, WIIMOTE_SDP_UINT16, WIIMOTE_DEVICE_ID_VERSION},
};

wiimote_sdp_values_t wiimote_sdp_hid_values(void)
{
    return (wiimote_sdp_values_t) {
        .values = s_hid_values,
        .count = sizeof(s_hid_values) / sizeof(s_hid_values[0]),
    };
}

wiimote_sdp_values_t wiimote_sdp_device_id_values(void)
{
    return (wiimote_sdp_values_t) {
        .values = s_device_id_values,
        .count = sizeof(s_device_id_values) / sizeof(s_device_id_values[0]),
    };
}

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "wiimote_sdp_values.h"

static const wiimote_sdp_value_t *find(wiimote_sdp_values_t attributes, uint16_t id)
{
    for (size_t i = 0; i < attributes.count; ++i) {
        if (attributes.values[i].id == id) return &attributes.values[i];
    }
    return NULL;
}

int main(void)
{
    wiimote_sdp_values_t hid = wiimote_sdp_hid_values();
    assert(hid.count == 8);
    assert(find(hid, 0x0202)->type == WIIMOTE_SDP_UINT8);
    assert(find(hid, 0x0202)->value == 0x04);
    assert(find(hid, 0x0203)->value == 0x33);
    assert(find(hid, 0x0204)->value == 0);
    assert(find(hid, 0x0208)->value == 0);
    assert(find(hid, 0x020a)->value == 1);
    assert(find(hid, 0x020c)->type == WIIMOTE_SDP_UINT16);
    assert(find(hid, 0x020c)->value == 0x0c80);
    assert(find(hid, 0x020d)->value == 0);
    assert(find(hid, 0x020e)->value == 0);
    for (size_t i = 0; i < hid.count; ++i) {
        if (hid.values[i].type == WIIMOTE_SDP_BOOLEAN) assert(hid.values[i].value <= 1);
        if (hid.values[i].type == WIIMOTE_SDP_UINT8) assert(hid.values[i].value <= UINT8_MAX);
        for (size_t j = i + 1; j < hid.count; ++j) {
            assert(hid.values[i].id != hid.values[j].id);
        }
    }

    wiimote_sdp_values_t device_id = wiimote_sdp_device_id_values();
    assert(device_id.count == 2);
    assert(find(device_id, 0x0200)->value == 0x0100);
    assert(find(device_id, 0x0203)->value == 0x8500);
    assert(WIIMOTE_DEVICE_ID_VERSION == 0x8500);
    puts("Captured wiimote HID flags, country/subclass, supervision timeout, and Device ID version: PASS");
    return 0;
}

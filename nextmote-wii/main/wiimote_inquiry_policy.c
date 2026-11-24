#include "wiimote_inquiry_policy.h"

#include <string.h>

static const wiimote_iac_t s_giac = {0x9e, 0x8b, 0x33};
static const wiimote_iac_t s_liac = {0x9e, 0x8b, 0x00};

bool wiimote_inquiry_parse_capacity(const uint8_t *event, size_t length,
                                    uint8_t *capacity)
{
    if (!event || !capacity || length != 7 || event[0] != 0x0e ||
        event[1] != 5 || event[3] != 0x38 || event[4] != 0x0c ||
        event[5] != 0 || event[6] == 0 || event[6] > 0x40) {
        return false;
    }
    *capacity = event[6];
    return true;
}

bool wiimote_inquiry_select_codes(uint8_t capacity, uint8_t count,
                                  const wiimote_iac_t *requested,
                                  wiimote_iac_t *selected, uint8_t *selected_count)
{
    if (!capacity || !count || count > 2 || !requested ||
        !selected || !selected_count) {
        return false;
    }
    if (count <= capacity) {
        memcpy(selected, requested, count * sizeof(wiimote_iac_t));
        *selected_count = count;
        return true;
    }

    bool limited_first = memcmp(requested[0], s_liac, sizeof(s_liac)) == 0 &&
                         memcmp(requested[1], s_giac, sizeof(s_giac)) == 0;
    bool general_first = memcmp(requested[0], s_giac, sizeof(s_giac)) == 0 &&
                         memcmp(requested[1], s_liac, sizeof(s_liac)) == 0;
    if (capacity != 1 || count != 2 || (!limited_first && !general_first)) {
        return false;
    }
    memcpy(selected[0], s_giac, sizeof(s_giac));
    *selected_count = 1;
    return true;
}

void wiimote_inquiry_get_code(bool general, wiimote_iac_t code)
{
    memcpy(code, general ? s_giac : s_liac, sizeof(wiimote_iac_t));
}

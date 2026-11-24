#include <inttypes.h>
#include <stdio.h>

const char *wiimote_bt_address_string(uint8_t address[6], char out[18])
{
    if (address == NULL || out == NULL) return NULL;
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             address[0], address[1], address[2], address[3], address[4], address[5]);
    return out;
}

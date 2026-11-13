#include "nunchuk_adapter_protocol.h"

#include <stddef.h>

void nunchuk_adapter_decode(const uint8_t raw[NUNCHUK_ADAPTER_DATA_LEN],
                            wiimote_nunchuk_input_t *input)
{
    if (raw == NULL || input == NULL) return;

    input->stick_x = raw[0];
    input->stick_y = raw[1];
    input->accel[0] = ((uint16_t)raw[2] << 2) | ((raw[5] >> 6) & 0x03U);
    input->accel[1] = ((uint16_t)raw[3] << 2) | ((raw[5] >> 4) & 0x03U);
    input->accel[2] = ((uint16_t)raw[4] << 2) | ((raw[5] >> 2) & 0x03U);
    input->c = (raw[5] & 0x02U) == 0;
    input->z = (raw[5] & 0x01U) == 0;
}

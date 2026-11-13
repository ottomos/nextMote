#pragma once

#include <stdint.h>

#include "wiimote_nunchuk.h"

#define NUNCHUK_ADAPTER_DATA_LEN 6

void nunchuk_adapter_decode(const uint8_t raw[NUNCHUK_ADAPTER_DATA_LEN],
                            wiimote_nunchuk_input_t *input);

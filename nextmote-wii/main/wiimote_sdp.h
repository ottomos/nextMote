#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

bool wiimote_sdp_hid_record_is_ready(void);
void wiimote_sdp_clear_hid_ready(void);
esp_err_t wiimote_sdp_apply_device_id(uint32_t handle);

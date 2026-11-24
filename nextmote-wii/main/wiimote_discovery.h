#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef void (*wiimote_discovery_error_callback_t)(esp_err_t error);

esp_err_t wiimote_discovery_init(wiimote_discovery_error_callback_t error_callback);
esp_err_t wiimote_discovery_set_rotation(bool enabled);

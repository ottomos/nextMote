#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

esp_err_t wiimote_physical_buttons_start(void);

typedef struct {
    bool enabled;
    uint16_t mask;
    uint32_t presses[6];
    uint32_t releases[6];
} wiimote_physical_buttons_snapshot_t;

void wiimote_physical_buttons_get_snapshot(wiimote_physical_buttons_snapshot_t *snapshot);

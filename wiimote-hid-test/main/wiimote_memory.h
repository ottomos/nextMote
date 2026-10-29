#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t wiimote_memory_start(void);
void wiimote_memory_reset(void);
uint8_t wiimote_memory_handle_read(const uint8_t *payload, uint16_t length);
uint8_t wiimote_memory_handle_write(const uint8_t *payload, uint16_t length);

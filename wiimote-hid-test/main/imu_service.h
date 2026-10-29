#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool detected;
    bool available;
    uint8_t address;
    float roll_deg;
    float pitch_deg;
    float yaw_deg;
} imu_state_t;

esp_err_t imu_service_start(void);
void imu_service_get_state(imu_state_t *state);

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define IR_CAMERA_POINT_COUNT 4
#define IR_CAMERA_NO_POINT 1023

typedef struct {
    bool detected;
    bool available;
    uint16_t x[IR_CAMERA_POINT_COUNT];
    uint16_t y[IR_CAMERA_POINT_COUNT];
} ir_camera_state_t;

esp_err_t ir_camera_init(void);
esp_err_t ir_camera_read(ir_camera_state_t *state);

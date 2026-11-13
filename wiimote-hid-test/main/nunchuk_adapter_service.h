#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    NUNCHUK_INPUT_SOURCE_SIMULATOR = 0,
    NUNCHUK_INPUT_SOURCE_ADAPTER,
} nunchuk_input_source_t;

typedef enum {
    NUNCHUK_READ_STAGE_NONE = 0,
    NUNCHUK_READ_STAGE_REGISTER_SELECT,
    NUNCHUK_READ_STAGE_DATA_READ,
} nunchuk_read_stage_t;

typedef struct {
    nunchuk_input_source_t source;
    bool detected;
    bool initialized;
    uint8_t raw[8];
    uint32_t samples;
    uint32_t read_failures;
    uint32_t read_attempt_errors;
    esp_err_t last_error;
    esp_err_t last_read_error;
    nunchuk_read_stage_t last_read_stage;
} nunchuk_adapter_status_t;

esp_err_t nunchuk_adapter_service_start(void);
esp_err_t nunchuk_adapter_service_set_source(nunchuk_input_source_t source);
void nunchuk_adapter_service_get_status(nunchuk_adapter_status_t *status);

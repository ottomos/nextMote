#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

esp_err_t sensor_i2c_bus_get(i2c_master_bus_handle_t *bus);
esp_err_t sensor_i2c_bus_lock(TickType_t timeout_ticks);
void sensor_i2c_bus_unlock(void);

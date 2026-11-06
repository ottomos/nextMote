#pragma once

#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

typedef enum {
    SENSOR_I2C_USER_CAMERA = 0,
    SENSOR_I2C_USER_IMU,
    SENSOR_I2C_USER_OTHER,
    SENSOR_I2C_USER_COUNT,
} sensor_i2c_user_t;

/* Diagnostic lock timing per bus user. Times are microseconds. */
typedef struct {
    uint32_t locks;
    uint32_t timeouts;
    uint32_t wait_max_us;
    uint64_t wait_sum_us;
    uint32_t waits_over_20ms;
    uint32_t waits_over_100ms;
    uint32_t hold_max_us;
    uint64_t hold_sum_us;
    uint32_t holds_over_20ms;
    uint32_t holds_over_100ms;
} sensor_i2c_user_stats_t;

typedef struct {
    sensor_i2c_user_stats_t user[SENSOR_I2C_USER_COUNT];
    uint32_t imu_packets;
    uint32_t imu_packet_max_us;
    uint64_t imu_packet_sum_us;
    uint32_t imu_packets_over_5ms;
    uint32_t imu_packets_over_20ms;
    int64_t since_us;
} sensor_i2c_bus_stats_t;

esp_err_t sensor_i2c_bus_get(i2c_master_bus_handle_t *bus);
esp_err_t sensor_i2c_bus_lock(TickType_t timeout_ticks);
esp_err_t sensor_i2c_bus_lock_as(sensor_i2c_user_t user, TickType_t timeout_ticks);
void sensor_i2c_bus_unlock(void);

void sensor_i2c_bus_record_imu_packet(uint32_t duration_us);
void sensor_i2c_bus_get_stats(sensor_i2c_bus_stats_t *stats);
void sensor_i2c_bus_reset_stats(void);

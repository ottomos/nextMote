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
    bool accel_available;
    float accel_ms2[3];
    uint32_t accel_samples;
    int64_t accel_time_us;
    uint32_t recoveries;
    bool gyro_available;
    float gyro_rad_s[3];
    uint32_t gyro_samples;
    int64_t gyro_time_us;
} imu_state_t;

#define IMU_ACCEL_MAX_AGE_US 250000

static inline bool imu_service_accel_is_fresh(const imu_state_t *state, int64_t now_us)
{
    return state->accel_available && now_us >= state->accel_time_us &&
           now_us - state->accel_time_us <= IMU_ACCEL_MAX_AGE_US;
}

static inline bool imu_service_gyro_is_fresh(const imu_state_t *state, int64_t now_us)
{
    return state->gyro_available && now_us >= state->gyro_time_us &&
           now_us - state->gyro_time_us <= IMU_ACCEL_MAX_AGE_US;
}

esp_err_t imu_service_start(void);
void imu_service_get_state(imu_state_t *state);

#include "imu_service.h"

#include <math.h>
#include <stddef.h>

#include "bno085.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "sensor_i2c_bus.h"
#include "wiimote_motion_plus.h"

#define IMU_I2C_SPEED_HZ 400000
#define IMU_I2C_TIMEOUT_MS 100
#define IMU_ACCEL_INTERVAL_US 10000
/* Orientation only feeds the web page; 50 Hz halves its bus traffic. */
#define IMU_ROTATION_INTERVAL_US 20000
#define IMU_GYRO_INTERVAL_US 10000
#define IMU_SERVICE_PERIOD_MS 5
/* Bound one bus hold so other I2C users (IR camera) are not starved. */
#define IMU_MAX_PACKETS_PER_LOCK 8
#define IMU_STALL_US 500000
#define IMU_REINIT_AFTER_STALLS 3
#define IMU_INT_GPIO GPIO_NUM_27
#define IMU_RESET_GPIO GPIO_NUM_26

static const char *TAG = "imu_service";
/* 7-bit addresses used by the legacy tracker and the component's example Kconfig. */
static const uint8_t s_bno_addresses[] = {0x4b, 0x4a};
static bno085_handle_t s_bno;
static i2c_master_dev_handle_t s_bno_device;
static bool s_started;
static imu_state_t s_state;
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;

static void publish_rotation_vector(const bno085_sensor_value_t *value)
{
    const float qi = value->data.quaternion.i;
    const float qj = value->data.quaternion.j;
    const float qk = value->data.quaternion.k;
    const float qr = value->data.quaternion.real;

    const float sinr_cosp = 2.0f * (qr * qi + qj * qk);
    const float cosr_cosp = 1.0f - 2.0f * (qi * qi + qj * qj);
    const float roll = atan2f(sinr_cosp, cosr_cosp);

    const float sinp = 2.0f * (qr * qj - qk * qi);
    const float pitch = fabsf(sinp) >= 1.0f ?
                            copysignf(1.5707963f, sinp) :
                            asinf(sinp);

    const float siny_cosp = 2.0f * (qr * qk + qi * qj);
    const float cosy_cosp = 1.0f - 2.0f * (qj * qj + qk * qk);
    const float yaw = atan2f(siny_cosp, cosy_cosp);
    const float radians_to_degrees = 57.2957795f;

    portENTER_CRITICAL(&s_state_mux);
    s_state.available = true;
    s_state.roll_deg = roll * radians_to_degrees;
    s_state.pitch_deg = pitch * radians_to_degrees;
    s_state.yaw_deg = yaw * radians_to_degrees;
    portEXIT_CRITICAL(&s_state_mux);
}

static void sensor_callback(bno085_handle_t handle,
                            const bno085_sensor_value_t *value,
                            void *user_context)
{
    (void)handle;
    (void)user_context;
    if (value == NULL) return;
    if (value->sensor_id == BNO085_SENSOR_ROTATION_VECTOR) {
        publish_rotation_vector(value);
    } else if (value->sensor_id == BNO085_SENSOR_ACCELEROMETER) {
        const float x = value->data.accelerometer.x;
        const float y = value->data.accelerometer.y;
        const float z = value->data.accelerometer.z;
        const bool valid = isfinite(x) && isfinite(y) && isfinite(z);
        const int64_t now = esp_timer_get_time();
        portENTER_CRITICAL(&s_state_mux);
        s_state.accel_available = valid;
        if (valid) {
            s_state.accel_ms2[0] = x;
            s_state.accel_ms2[1] = y;
            s_state.accel_ms2[2] = z;
            s_state.accel_time_us = now;
            ++s_state.accel_samples;
        }
        portEXIT_CRITICAL(&s_state_mux);
        if (!valid) ESP_LOGE(TAG, "Invalid non-finite accelerometer sample");
    } else if (value->sensor_id == BNO085_SENSOR_GYROSCOPE_CALIBRATED) {
        const float x = value->data.gyroscope_calibrated.x;
        const float y = value->data.gyroscope_calibrated.y;
        const float z = value->data.gyroscope_calibrated.z;
        const bool valid = isfinite(x) && isfinite(y) && isfinite(z);
        const int64_t now = esp_timer_get_time();
        portENTER_CRITICAL(&s_state_mux);
        s_state.gyro_available = valid;
        if (valid) {
            s_state.gyro_rad_s[0] = x;
            s_state.gyro_rad_s[1] = y;
            s_state.gyro_rad_s[2] = z;
            s_state.gyro_time_us = now;
            ++s_state.gyro_samples;
        }
        portEXIT_CRITICAL(&s_state_mux);
    }
}

static esp_err_t enable_reports(bno085_handle_t handle)
{
    esp_err_t err = bno085_enable_sensor(handle, BNO085_SENSOR_ROTATION_VECTOR,
                                         IMU_ROTATION_INTERVAL_US);
    if (err == ESP_OK) {
        err = bno085_enable_sensor(handle, BNO085_SENSOR_ACCELEROMETER,
                                   IMU_ACCEL_INTERVAL_US);
    }
#if NEXTMOTE_ENABLE_MOTION_PLUS
    if (err == ESP_OK) {
        err = bno085_enable_sensor(handle, BNO085_SENSOR_GYROSCOPE_CALIBRATED,
                                   IMU_GYRO_INTERVAL_US);
    }
#endif
    return err;
}

static esp_err_t open_bno(i2c_master_dev_handle_t device, bno085_handle_t *out)
{
    bno085_config_t config;
    bno085_config_default(&config);
    bno085_handle_t handle = NULL;
    esp_err_t err = bno085_init(&config, device, IMU_INT_GPIO, IMU_RESET_GPIO, &handle);
    if (err != ESP_OK) return err;
    err = bno085_register_sensor_callback(handle, sensor_callback, NULL);
    if (err == ESP_OK) err = enable_reports(handle);
    if (err != ESP_OK) {
        bno085_deinit(handle);
        return err;
    }
    *out = handle;
    return ESP_OK;
}

static uint32_t accel_sample_count(void)
{
    portENTER_CRITICAL(&s_state_mux);
    const uint32_t samples = s_state.accel_samples;
    portEXIT_CRITICAL(&s_state_mux);
    return samples;
}

/*
 * The BNO08x can reset itself (watchdog, bus glitch); afterwards every report
 * is disabled and the driver only logs the reset. Re-enable the reports when
 * accelerometer samples stop, and fully re-initialize after repeated stalls.
 */
static void recover_stall(unsigned *stalls)
{
    ++*stalls;
    portENTER_CRITICAL(&s_state_mux);
    ++s_state.recoveries;
    portEXIT_CRITICAL(&s_state_mux);

    if (*stalls < IMU_REINIT_AFTER_STALLS) {
        ESP_LOGW(TAG, "BNO08x stream stalled; re-enabling reports (attempt %u)", *stalls);
        esp_err_t err = enable_reports(s_bno);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Re-enabling reports failed: %s", esp_err_to_name(err));
        }
        return;
    }

    ESP_LOGW(TAG, "BNO08x stream still stalled; resetting and re-initializing");
    bno085_deinit(s_bno);
    s_bno = NULL;
    esp_err_t err = open_bno(s_bno_device, &s_bno);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BNO08x re-initialization failed: %s", esp_err_to_name(err));
    }
    *stalls = 0;
}

static void imu_service_task(void *arg)
{
    (void)arg;
    uint32_t last_samples = accel_sample_count();
    int64_t last_progress_us = esp_timer_get_time();
    unsigned stalls = 0;

    for (;;) {
        esp_err_t err = sensor_i2c_bus_lock(portMAX_DELAY);
        if (err == ESP_OK) {
            if (s_bno != NULL) {
                /* The HAL reads one SHTP packet per call and only while INT is low. */
                int packets = 0;
                do {
                    bno085_service(s_bno);
                } while (++packets < IMU_MAX_PACKETS_PER_LOCK &&
                         gpio_get_level(IMU_INT_GPIO) == 0);
            }

            const uint32_t samples = accel_sample_count();
            const int64_t now = esp_timer_get_time();
            if (samples != last_samples || s_bno == NULL) {
                if (samples != last_samples) stalls = 0;
                last_samples = samples;
                last_progress_us = now;
            } else if (now - last_progress_us > IMU_STALL_US) {
                recover_stall(&stalls);
                last_progress_us = esp_timer_get_time();
            }
            sensor_i2c_bus_unlock();
        } else {
            ESP_LOGE(TAG, "Failed to lock shared I2C bus: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(IMU_SERVICE_PERIOD_MS));
    }
}

void imu_service_get_state(imu_state_t *state)
{
    if (state == NULL) return;
    portENTER_CRITICAL(&s_state_mux);
    *state = s_state;
    portEXIT_CRITICAL(&s_state_mux);
    const int64_t now = esp_timer_get_time();
    state->accel_available = imu_service_accel_is_fresh(state, now);
    state->gyro_available = imu_service_gyro_is_fresh(state, now);
}

esp_err_t imu_service_start(void)
{
    if (s_started) return ESP_ERR_INVALID_STATE;

    i2c_master_bus_handle_t bus;
    esp_err_t err = sensor_i2c_bus_get(&bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Unable to initialize shared I2C bus: %s", esp_err_to_name(err));
        return err;
    }

    err = sensor_i2c_bus_lock(pdMS_TO_TICKS(IMU_I2C_TIMEOUT_MS));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Unable to lock shared I2C bus: %s", esp_err_to_name(err));
        return err;
    }

    bno085_handle_t handle = NULL;
    i2c_master_dev_handle_t device = NULL;
    uint8_t detected_address = 0;
    esp_err_t last_error = ESP_ERR_NOT_FOUND;

    for (size_t i = 0; i < sizeof(s_bno_addresses) / sizeof(s_bno_addresses[0]); ++i) {
        const uint8_t address = s_bno_addresses[i];
        err = i2c_master_probe(bus, address, IMU_I2C_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGI(TAG, "No BNO08x response at 0x%02x: %s",
                     address, esp_err_to_name(err));
            last_error = err;
            continue;
        }

        const i2c_device_config_t device_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = address,
            .scl_speed_hz = IMU_I2C_SPEED_HZ,
        };
        err = i2c_master_bus_add_device(bus, &device_config, &device);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to add BNO08x at 0x%02x: %s",
                     address, esp_err_to_name(err));
            last_error = err;
            continue;
        }

        err = open_bno(device, &handle);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "BNO08x setup failed at 0x%02x: %s",
                     address, esp_err_to_name(err));
            (void)i2c_master_bus_rm_device(device);
            device = NULL;
            last_error = err;
            continue;
        }

        detected_address = address;
        last_error = ESP_OK;
        break;
    }

    if (last_error != ESP_OK) {
        sensor_i2c_bus_unlock();
        portENTER_CRITICAL(&s_state_mux);
        s_state.detected = false;
        s_state.available = false;
        s_state.address = 0;
        portEXIT_CRITICAL(&s_state_mux);
        ESP_LOGW(TAG, "BNO08x not detected at I2C addresses 0x4b or 0x4a");
        return last_error == ESP_ERR_NOT_FOUND ? ESP_ERR_NOT_FOUND : last_error;
    }

    s_bno = handle;
    s_bno_device = device;
    portENTER_CRITICAL(&s_state_mux);
    s_state.detected = true;
    s_state.available = false;
    s_state.address = detected_address;
    portEXIT_CRITICAL(&s_state_mux);

    BaseType_t task_result = xTaskCreate(imu_service_task, "imu_service", 4096,
                                         NULL, 5, NULL);
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create IMU service task");
        bno085_deinit(s_bno);
        (void)i2c_master_bus_rm_device(s_bno_device);
        s_bno = NULL;
        s_bno_device = NULL;
        portENTER_CRITICAL(&s_state_mux);
        s_state.detected = false;
        s_state.available = false;
        s_state.address = 0;
        portEXIT_CRITICAL(&s_state_mux);
        sensor_i2c_bus_unlock();
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    sensor_i2c_bus_unlock();
    ESP_LOGI(TAG, "BNO08x initialized at 0x%02x; acceleration 100 Hz, rotation vector 50 Hz, I2C %u kHz",
             detected_address, IMU_I2C_SPEED_HZ / 1000);
    return ESP_OK;
}

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
#include "nextmote_cores.h"

#define IMU_I2C_SPEED_HZ 100000
#define IMU_I2C_TIMEOUT_MS 100
#define IMU_REPORT_INTERVAL_US 10000
#define IMU_SERVICE_PERIOD_MS 10
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
    }
}

static void imu_service_task(void *arg)
{
    bno085_handle_t handle = (bno085_handle_t)arg;

    for (;;) {
        esp_err_t err = sensor_i2c_bus_lock(portMAX_DELAY);
        if (err == ESP_OK) {
            bno085_service(handle);
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
    state->accel_available = imu_service_accel_is_fresh(state, esp_timer_get_time());
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

        bno085_config_t config;
        bno085_config_default(&config);
        err = bno085_init(&config, device, IMU_INT_GPIO, IMU_RESET_GPIO, &handle);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "BNO08x initialization failed at 0x%02x: %s",
                     address, esp_err_to_name(err));
            (void)i2c_master_bus_rm_device(device);
            device = NULL;
            last_error = err;
            continue;
        }

        err = bno085_register_sensor_callback(handle, sensor_callback, NULL);
        if (err == ESP_OK) {
            err = bno085_enable_sensor(handle, BNO085_SENSOR_ROTATION_VECTOR,
                                       IMU_REPORT_INTERVAL_US);
        }
        if (err == ESP_OK) {
            err = bno085_enable_sensor(handle, BNO085_SENSOR_ACCELEROMETER,
                                       IMU_REPORT_INTERVAL_US);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to enable BNO08x orientation/accelerometer: %s",
                     esp_err_to_name(err));
            bno085_deinit(handle);
            handle = NULL;
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

    BaseType_t task_result = xTaskCreatePinnedToCore(imu_service_task, "imu_service", 4096,
                                         (void *)s_bno, 5, NULL, NEXTMOTE_APP_CORE);
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
    ESP_LOGI(TAG, "BNO08x initialized at 0x%02x; rotation vector and gravity-inclusive acceleration enabled at 100 Hz",
             detected_address);
    return ESP_OK;
}

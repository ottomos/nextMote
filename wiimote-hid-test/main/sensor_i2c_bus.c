#include "sensor_i2c_bus.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/semphr.h"

#define SENSOR_I2C_PORT I2C_NUM_0
#define SENSOR_I2C_SDA_PIN GPIO_NUM_21
#define SENSOR_I2C_SCL_PIN GPIO_NUM_22

static const char *TAG = "sensor_i2c_bus";
static i2c_master_bus_handle_t s_bus;
static SemaphoreHandle_t s_bus_lock;

esp_err_t sensor_i2c_bus_get(i2c_master_bus_handle_t *bus)
{
    if (bus == NULL) return ESP_ERR_INVALID_ARG;

    if (s_bus == NULL) {
        if (s_bus_lock == NULL) {
            s_bus_lock = xSemaphoreCreateMutex();
            if (s_bus_lock == NULL) {
                ESP_LOGE(TAG, "Failed to create shared I2C bus mutex");
                return ESP_ERR_NO_MEM;
            }
        }

        const i2c_master_bus_config_t config = {
            .i2c_port = SENSOR_I2C_PORT,
            .sda_io_num = SENSOR_I2C_SDA_PIN,
            .scl_io_num = SENSOR_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        esp_err_t err = i2c_new_master_bus(&config, &s_bus);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create shared I2C bus: %s", esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "Shared I2C bus ready on SDA=21 SCL=22");
    }

    *bus = s_bus;
    return ESP_OK;
}

esp_err_t sensor_i2c_bus_lock(TickType_t timeout_ticks)
{
    if (s_bus_lock == NULL) return ESP_ERR_INVALID_STATE;
    return xSemaphoreTake(s_bus_lock, timeout_ticks) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

void sensor_i2c_bus_unlock(void)
{
    if (s_bus_lock == NULL || xSemaphoreGive(s_bus_lock) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to release shared I2C bus mutex");
    }
}

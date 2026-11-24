#include "sensor_i2c_bus.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define SENSOR_I2C_PORT I2C_NUM_0
#define SENSOR_I2C_SDA_PIN GPIO_NUM_21
#define SENSOR_I2C_SCL_PIN GPIO_NUM_22

static const char *TAG = "sensor_i2c_bus";
static i2c_master_bus_handle_t s_bus;
static SemaphoreHandle_t s_bus_lock;
static portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;
static sensor_i2c_bus_stats_t s_stats;
/* Only touched by the current mutex holder. */
static sensor_i2c_user_t s_holder = SENSOR_I2C_USER_OTHER;
static int64_t s_hold_start_us;

#define SENSOR_I2C_ISR_CORE 1

typedef struct {
    TaskHandle_t caller;
    esp_err_t result;
} bus_create_request_t;

/* The I2C ISR is allocated on the core that creates the bus. Bluetooth and Wi-Fi run on
 * core 0; under game traffic their load delayed the per-chunk I2C refill interrupts and
 * stretched long BNO08x packet reads. Only the bus creation is pinned here. */
static void create_bus_task(void *arg)
{
    bus_create_request_t *request = arg;
    const i2c_master_bus_config_t config = {
        .i2c_port = SENSOR_I2C_PORT,
        .sda_io_num = SENSOR_I2C_SDA_PIN,
        .scl_io_num = SENSOR_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    request->result = i2c_new_master_bus(&config, &s_bus);
    xTaskNotifyGive(request->caller);
    vTaskDelete(NULL);
}

static esp_err_t create_bus_on_sensor_core(void)
{
    bus_create_request_t request = {
        .caller = xTaskGetCurrentTaskHandle(),
        .result = ESP_FAIL,
    };
    if (xTaskCreatePinnedToCore(create_bus_task, "i2c_bus_init", 3072, &request,
                                uxTaskPriorityGet(NULL), NULL,
                                SENSOR_I2C_ISR_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return request.result;
}

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

        esp_err_t err = create_bus_on_sensor_core();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create shared I2C bus: %s", esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "Shared I2C bus ready on SDA=21 SCL=22 (ISR on core %d)", SENSOR_I2C_ISR_CORE);
    }

    *bus = s_bus;
    return ESP_OK;
}

esp_err_t sensor_i2c_bus_lock(TickType_t timeout_ticks)
{
    return sensor_i2c_bus_lock_as(SENSOR_I2C_USER_OTHER, timeout_ticks);
}

esp_err_t sensor_i2c_bus_lock_as(sensor_i2c_user_t user, TickType_t timeout_ticks)
{
    if (s_bus_lock == NULL) return ESP_ERR_INVALID_STATE;
    if ((unsigned)user >= SENSOR_I2C_USER_COUNT) user = SENSOR_I2C_USER_OTHER;

    const int64_t start = esp_timer_get_time();
    const bool taken = xSemaphoreTake(s_bus_lock, timeout_ticks) == pdTRUE;
    const int64_t now = esp_timer_get_time();
    const uint32_t wait = (uint32_t)(now - start);

    portENTER_CRITICAL(&s_stats_mux);
    sensor_i2c_user_stats_t *st = &s_stats.user[user];
    if (taken) {
        st->locks++;
        st->wait_sum_us += wait;
        if (wait > st->wait_max_us) st->wait_max_us = wait;
        if (wait > 20000) st->waits_over_20ms++;
        if (wait > 100000) st->waits_over_100ms++;
    } else {
        st->timeouts++;
    }
    portEXIT_CRITICAL(&s_stats_mux);

    if (!taken) return ESP_ERR_TIMEOUT;
    s_holder = user;
    s_hold_start_us = now;
    return ESP_OK;
}

void sensor_i2c_bus_unlock(void)
{
    if (s_bus_lock == NULL) {
        ESP_LOGE(TAG, "Failed to release shared I2C bus mutex");
        return;
    }
    const uint32_t hold = (uint32_t)(esp_timer_get_time() - s_hold_start_us);
    const sensor_i2c_user_t user = s_holder;
    s_holder = SENSOR_I2C_USER_OTHER;

    portENTER_CRITICAL(&s_stats_mux);
    sensor_i2c_user_stats_t *st = &s_stats.user[user];
    st->hold_sum_us += hold;
    if (hold > st->hold_max_us) st->hold_max_us = hold;
    if (hold > 20000) st->holds_over_20ms++;
    if (hold > 100000) st->holds_over_100ms++;
    portEXIT_CRITICAL(&s_stats_mux);

    if (xSemaphoreGive(s_bus_lock) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to release shared I2C bus mutex");
    }
}

void sensor_i2c_bus_record_imu_packet(uint32_t duration_us)
{
    portENTER_CRITICAL(&s_stats_mux);
    s_stats.imu_packets++;
    s_stats.imu_packet_sum_us += duration_us;
    if (duration_us > s_stats.imu_packet_max_us) s_stats.imu_packet_max_us = duration_us;
    if (duration_us > 5000) s_stats.imu_packets_over_5ms++;
    if (duration_us > 20000) s_stats.imu_packets_over_20ms++;
    portEXIT_CRITICAL(&s_stats_mux);
}

void sensor_i2c_bus_get_stats(sensor_i2c_bus_stats_t *stats)
{
    if (stats == NULL) return;
    portENTER_CRITICAL(&s_stats_mux);
    *stats = s_stats;
    portEXIT_CRITICAL(&s_stats_mux);
}

void sensor_i2c_bus_reset_stats(void)
{
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_stats_mux);
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.since_us = now;
    portEXIT_CRITICAL(&s_stats_mux);
}

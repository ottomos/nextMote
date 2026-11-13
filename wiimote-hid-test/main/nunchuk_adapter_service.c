#include "nunchuk_adapter_service.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "nunchuk_adapter_protocol.h"
#include "sensor_i2c_bus.h"
#include "wiimote_nunchuk.h"

#define NUNCHUK_I2C_ADDRESS 0x52
#define NUNCHUK_I2C_CLOCK_HZ 100000
#define NUNCHUK_I2C_TIMEOUT_MS 25
#define NUNCHUK_PROBE_RETRY_MS 1000
#define NUNCHUK_POLL_MS 20
#define NUNCHUK_READ_RETRY_LIMIT 2
#define NUNCHUK_READ_RETRY_DELAY_US 5000
#define NUNCHUK_REGISTER_READ_DELAY_US 2000
#define NUNCHUK_READ_ERROR_LIMIT 10
#define NUNCHUK_ADAPTER_READ_LEN 8

static const char *TAG = "nunchuk_adapter";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_device;
static nunchuk_adapter_status_t s_status = {
    .source = NUNCHUK_INPUT_SOURCE_SIMULATOR,
    .last_error = ESP_ERR_NOT_FOUND,
};
static portMUX_TYPE s_status_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_started;

void nunchuk_adapter_service_get_status(nunchuk_adapter_status_t *status)
{
    if (status == NULL) return;
    portENTER_CRITICAL(&s_status_mux);
    *status = s_status;
    portEXIT_CRITICAL(&s_status_mux);
}

esp_err_t nunchuk_adapter_service_set_source(nunchuk_input_source_t source)
{
    if (source != NUNCHUK_INPUT_SOURCE_SIMULATOR &&
        source != NUNCHUK_INPUT_SOURCE_ADAPTER) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_status_mux);
    s_status.source = source;
    portEXIT_CRITICAL(&s_status_mux);

    if (source == NUNCHUK_INPUT_SOURCE_ADAPTER) {
        wiimote_nunchuk_input_t neutral;
        wiimote_nunchuk_neutral_input(&neutral);
        wiimote_nunchuk_set_input(&neutral);
    }
    return ESP_OK;
}

static esp_err_t lock_bus(void)
{
    return sensor_i2c_bus_lock_as(SENSOR_I2C_USER_NUNCHUK,
                                  pdMS_TO_TICKS(NUNCHUK_I2C_TIMEOUT_MS));
}

static esp_err_t ensure_device(void)
{
    if (s_device != NULL) return ESP_OK;

    esp_err_t err = sensor_i2c_bus_get(&s_bus);
    if (err != ESP_OK) return err;

    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = NUNCHUK_I2C_ADDRESS,
        .scl_speed_hz = NUNCHUK_I2C_CLOCK_HZ,
    };
    return i2c_master_bus_add_device(s_bus, &config, &s_device);
}

static esp_err_t transmit(const uint8_t *data, size_t length)
{
    esp_err_t err = lock_bus();
    if (err != ESP_OK) return err;
    err = i2c_master_transmit(s_device, data, length, NUNCHUK_I2C_TIMEOUT_MS);
    sensor_i2c_bus_unlock();
    return err;
}

static esp_err_t initialize_adapter(void)
{
    esp_err_t err = ensure_device();
    if (err != ESP_OK) return err;

    err = lock_bus();
    if (err != ESP_OK) return err;
    err = i2c_master_probe(s_bus, NUNCHUK_I2C_ADDRESS, NUNCHUK_I2C_TIMEOUT_MS);
    sensor_i2c_bus_unlock();
    if (err != ESP_OK) return err;

    static const uint8_t disable_encryption[] = {0xf0, 0x55};
    static const uint8_t select_plain_mode[] = {0xfb, 0x00};
    err = transmit(disable_encryption, sizeof(disable_encryption));
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(100));

    err = transmit(select_plain_mode, sizeof(select_plain_mode));
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

static esp_err_t read_sample(uint8_t raw[NUNCHUK_ADAPTER_READ_LEN],
                             nunchuk_read_stage_t *failed_stage)
{
    const uint8_t data_register = 0x00;
    *failed_stage = NUNCHUK_READ_STAGE_REGISTER_SELECT;
    esp_err_t err = transmit(&data_register, sizeof(data_register));
    if (err != ESP_OK) return err;

    esp_rom_delay_us(NUNCHUK_REGISTER_READ_DELAY_US);
    *failed_stage = NUNCHUK_READ_STAGE_DATA_READ;
    err = lock_bus();
    if (err != ESP_OK) return err;
    err = i2c_master_receive(s_device, raw, NUNCHUK_ADAPTER_READ_LEN,
                             NUNCHUK_I2C_TIMEOUT_MS);
    sensor_i2c_bus_unlock();
    return err;
}

static bool physical_source_selected(void)
{
    portENTER_CRITICAL(&s_status_mux);
    const bool selected = s_status.source == NUNCHUK_INPUT_SOURCE_ADAPTER;
    portEXIT_CRITICAL(&s_status_mux);
    return selected;
}

static void set_device_state(bool detected, bool initialized, esp_err_t error)
{
    portENTER_CRITICAL(&s_status_mux);
    s_status.detected = detected;
    s_status.initialized = initialized;
    s_status.last_error = error;
    if (!detected) memset(s_status.raw, 0, sizeof(s_status.raw));
    portEXIT_CRITICAL(&s_status_mux);
}

static void note_read_failure(esp_err_t error, nunchuk_read_stage_t stage,
                              unsigned consecutive_errors)
{
    portENTER_CRITICAL(&s_status_mux);
    ++s_status.read_failures;
    s_status.last_error = error;
    s_status.last_read_error = error;
    s_status.last_read_stage = stage;
    if (consecutive_errors >= NUNCHUK_READ_ERROR_LIMIT) {
        s_status.detected = false;
        s_status.initialized = false;
        memset(s_status.raw, 0, sizeof(s_status.raw));
    }
    portEXIT_CRITICAL(&s_status_mux);
}

static void note_read_attempt_error(esp_err_t error, nunchuk_read_stage_t stage)
{
    portENTER_CRITICAL(&s_status_mux);
    ++s_status.read_attempt_errors;
    s_status.last_read_error = error;
    s_status.last_read_stage = stage;
    portEXIT_CRITICAL(&s_status_mux);
}

static void note_sample(const uint8_t raw[NUNCHUK_ADAPTER_READ_LEN])
{
    portENTER_CRITICAL(&s_status_mux);
    s_status.detected = true;
    s_status.initialized = true;
    s_status.last_error = ESP_OK;
    memcpy(s_status.raw, raw, NUNCHUK_ADAPTER_READ_LEN);
    ++s_status.samples;
    portEXIT_CRITICAL(&s_status_mux);
}

static void adapter_task(void *arg)
{
    (void)arg;
    bool initialized = false;
    unsigned consecutive_errors = 0;
    esp_err_t last_logged_error = ESP_FAIL;
    int64_t last_read_warning_us = 0;
    for (;;) {
        if (!initialized) {
            const esp_err_t err = initialize_adapter();
            if (err != ESP_OK) {
                set_device_state(false, false, err);
                if (err != last_logged_error) {
                    if (err == ESP_ERR_NOT_FOUND) {
                        ESP_LOGI(TAG, "No Nunchuk detected at I2C 0x52; simulator remains available");
                    } else {
                        ESP_LOGW(TAG, "Adapter initialization failed: %s", esp_err_to_name(err));
                    }
                    last_logged_error = err;
                }
                vTaskDelay(pdMS_TO_TICKS(NUNCHUK_PROBE_RETRY_MS));
                continue;
            }
            initialized = true;
            consecutive_errors = 0;
            last_logged_error = ESP_OK;
            set_device_state(true, true, ESP_OK);
            ESP_LOGI(TAG, "Nunchuk adapter ready at I2C 0x52");
        }

        if (!physical_source_selected()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        uint8_t raw[NUNCHUK_ADAPTER_READ_LEN];
        nunchuk_read_stage_t failed_stage = NUNCHUK_READ_STAGE_NONE;
        esp_err_t err = ESP_FAIL;
        for (unsigned attempt = 0; attempt <= NUNCHUK_READ_RETRY_LIMIT; ++attempt) {
            failed_stage = NUNCHUK_READ_STAGE_NONE;
            err = read_sample(raw, &failed_stage);
            if (err == ESP_OK) break;
            note_read_attempt_error(err, failed_stage);
            if (attempt < NUNCHUK_READ_RETRY_LIMIT) {
                esp_rom_delay_us(NUNCHUK_READ_RETRY_DELAY_US);
            }
        }
        if (err != ESP_OK) {
            ++consecutive_errors;
            note_read_failure(err, failed_stage, consecutive_errors);
            const int64_t now_us = esp_timer_get_time();
            if (last_read_warning_us == 0 || now_us - last_read_warning_us >= 1000000) {
                ESP_LOGW(TAG, "Nunchuk %s failed: %s (%u consecutive)",
                         failed_stage == NUNCHUK_READ_STAGE_REGISTER_SELECT
                             ? "register select" : "data read",
                         esp_err_to_name(err), consecutive_errors);
                last_read_warning_us = now_us;
            }
            if (consecutive_errors >= NUNCHUK_READ_ERROR_LIMIT) {
                initialized = false;
                wiimote_nunchuk_input_t neutral;
                wiimote_nunchuk_neutral_input(&neutral);
                wiimote_nunchuk_set_input(&neutral);
            }
        } else {
            consecutive_errors = 0;
            note_sample(raw);
            if (physical_source_selected()) {
                wiimote_nunchuk_input_t input;
                nunchuk_adapter_decode(raw, &input);
                wiimote_nunchuk_set_input(&input);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(NUNCHUK_POLL_MS));
    }
}

esp_err_t nunchuk_adapter_service_start(void)
{
    if (s_started) return ESP_ERR_INVALID_STATE;
    s_started = true;
    if (xTaskCreate(adapter_task, "nunchuk_adapter", 3072, NULL, 5, NULL) != pdPASS) {
        s_started = false;
        ESP_LOGE(TAG, "Could not create adapter service task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/*
 * SEN0158 register protocol and coordinate encoding follow DFRobot's
 * DFRobotIRPosition documentation; its source header identifies an LGPL license.
 */
#include "ir_camera.h"

#include <stddef.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define IR_CAMERA_I2C_ADDRESS 0x58
#define IR_CAMERA_SDA_PIN GPIO_NUM_21
#define IR_CAMERA_SCL_PIN GPIO_NUM_22
#define IR_CAMERA_I2C_SPEED_HZ 100000
#define IR_CAMERA_TRANSFER_TIMEOUT_MS 100
#define IR_CAMERA_FRAME_SIZE 16
#define IR_CAMERA_REGISTER_POSITION 0x36

static const char *TAG = "ir_camera";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_device;
static bool s_initialized;

/* Keep the DFRobot SEN0158 library's stock initialization profile. */
static const uint8_t s_init_registers[][2] = {
    {0x30, 0x01},
    {0x30, 0x08},
    {0x06, 0x90},
    {0x08, 0xc0},
    {0x1a, 0x40},
    {0x33, 0x33},
};

static void release_handles(void)
{
    if (s_device != NULL) {
        esp_err_t err = i2c_master_bus_rm_device(s_device);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2C device cleanup failed: %s", esp_err_to_name(err));
        }
        s_device = NULL;
    }

    if (s_bus != NULL) {
        esp_err_t err = i2c_del_master_bus(s_bus);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2C bus cleanup failed: %s", esp_err_to_name(err));
        }
        s_bus = NULL;
    }
}

static void clear_points(ir_camera_state_t *state)
{
    for (size_t i = 0; i < IR_CAMERA_POINT_COUNT; ++i) {
        state->x[i] = IR_CAMERA_NO_POINT;
        state->y[i] = IR_CAMERA_NO_POINT;
    }
}

static esp_err_t write_register(uint8_t reg, uint8_t value)
{
    const uint8_t command[] = {reg, value};
    return i2c_master_transmit(s_device, command, sizeof(command), IR_CAMERA_TRANSFER_TIMEOUT_MS);
}

esp_err_t ir_camera_init(void)
{
    if (s_initialized || s_bus != NULL || s_device != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = IR_CAMERA_SDA_PIN,
        .scl_io_num = IR_CAMERA_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, &s_bus);
    if (err != ESP_OK) {
        return err;
    }

    err = i2c_master_probe(s_bus, IR_CAMERA_I2C_ADDRESS, IR_CAMERA_TRANSFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        release_handles();
        return err;
    }

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = IR_CAMERA_I2C_ADDRESS,
        .scl_speed_hz = IR_CAMERA_I2C_SPEED_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &device_config, &s_device);
    if (err != ESP_OK) {
        release_handles();
        return err;
    }

    for (size_t i = 0; i < sizeof(s_init_registers) / sizeof(s_init_registers[0]); ++i) {
        err = write_register(s_init_registers[i][0], s_init_registers[i][1]);
        if (err != ESP_OK) {
            release_handles();
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    vTaskDelay(pdMS_TO_TICKS(100));
    s_initialized = true;
    ESP_LOGI(TAG, "SEN0158 initialized with DFRobot default profile");
    return ESP_OK;
}

esp_err_t ir_camera_read(ir_camera_state_t *state)
{
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    state->detected = s_initialized;
    state->available = false;
    clear_points(state);
    if (!s_initialized || s_device == NULL) {
        state->detected = false;
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t position_register = IR_CAMERA_REGISTER_POSITION;
    esp_err_t err = i2c_master_transmit(s_device, &position_register, sizeof(position_register),
                                        IR_CAMERA_TRANSFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        state->detected = false;
        return err;
    }

    uint8_t frame[IR_CAMERA_FRAME_SIZE];
    err = i2c_master_receive(s_device, frame, sizeof(frame), IR_CAMERA_TRANSFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        state->detected = false;
        return err;
    }

    for (size_t i = 0; i < IR_CAMERA_POINT_COUNT; ++i) {
        const size_t offset = 1 + i * 3;
        const uint8_t high = frame[offset + 2];
        state->x[i] = (uint16_t)frame[offset] | ((uint16_t)(high & 0x30U) << 4);
        state->y[i] = (uint16_t)frame[offset + 1] | ((uint16_t)(high & 0xc0U) << 2);
    }

    state->available = true;
    return ESP_OK;
}

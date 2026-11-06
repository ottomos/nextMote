#include "wiimote_memory.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "esp_hidd_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "wiimote_protocol.h"
#include "wiimote_reports.h"
#include "wiimote_accel.h"
#include "nextmote_cores.h"

static const char *TAG = "wiimote_memory";

static const uint8_t s_wiimote_calibration[0x2a] = {
    0xa1, 0xaa, 0x8b, 0x99, 0xae, 0x9e, 0x78, 0x30, 0xa7, 0x74, 0xd3,
    0xa1, 0xaa, 0x8b, 0x99, 0xae, 0x9e, 0x78, 0x30, 0xa7, 0x74, 0xd3,
    WIIMOTE_ACCEL_ZERO_BYTE, WIIMOTE_ACCEL_ZERO_BYTE, WIIMOTE_ACCEL_ZERO_BYTE, 0x00,
    WIIMOTE_ACCEL_ONE_G_BYTE, WIIMOTE_ACCEL_ONE_G_BYTE, WIIMOTE_ACCEL_ONE_G_BYTE, 0x00, 0x40, 0xe3,
    WIIMOTE_ACCEL_ZERO_BYTE, WIIMOTE_ACCEL_ZERO_BYTE, WIIMOTE_ACCEL_ZERO_BYTE, 0x00,
    WIIMOTE_ACCEL_ONE_G_BYTE, WIIMOTE_ACCEL_ONE_G_BYTE, WIIMOTE_ACCEL_ONE_G_BYTE, 0x00, 0x40, 0xe3,
};

static const uint8_t s_empty_ir_camera_frame[WIIMOTE_IR_CAMERA_FRAME_LENGTH] = {
    0x00,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x00, 0x00,
};

static uint8_t s_ir_camera_registers[WIIMOTE_IR_CAMERA_REGISTER_COUNT];
static portMUX_TYPE s_ir_camera_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_memory_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    bool active;
    uint8_t space;
    uint8_t slave;
    uint16_t address;
    uint16_t remaining;
} s_memory_read;
static TaskHandle_t s_memory_read_task;

static uint8_t read_wiimote_memory(uint8_t space, uint8_t slave, uint16_t address,
                                   uint8_t length, uint8_t *output)
{
    if (space == WIIMOTE_MEMORY_SPACE_EEPROM) {
        if (address >= 0x1700 || (uint32_t)address + length > 0x1700)
            return WIIMOTE_MEMORY_ERROR_INVALID_ADDRESS;
        memset(output, 0, length);
        if (address < sizeof(s_wiimote_calibration)) {
            size_t calibration_length = sizeof(s_wiimote_calibration) - address;
            if (calibration_length > length) calibration_length = length;
            memcpy(output, s_wiimote_calibration + address, calibration_length);
        }
        return WIIMOTE_MEMORY_ERROR_SUCCESS;
    }

    if (space != WIIMOTE_MEMORY_SPACE_I2C &&
        space != WIIMOTE_MEMORY_SPACE_I2C_ALT) {
        return WIIMOTE_MEMORY_ERROR_INVALID_SPACE;
    }
    if (slave == WIIMOTE_EEPROM_I2C_ADDRESS)
        return WIIMOTE_MEMORY_ERROR_INVALID_ADDRESS;

    if (slave == WIIMOTE_I2C_SLAVE_IR_CAMERA) {
        if ((uint32_t)address + length > sizeof(s_ir_camera_registers))
            return WIIMOTE_MEMORY_ERROR_INVALID_ADDRESS;

        portENTER_CRITICAL(&s_ir_camera_mux);
        memcpy(output, &s_ir_camera_registers[address], length);
        portEXIT_CRITICAL(&s_ir_camera_mux);

        for (uint8_t i = 0; i < length; ++i) {
            uint16_t register_address = (uint16_t)(address + i);
            if (register_address >= WIIMOTE_IR_CAMERA_FRAME_REGISTER &&
                register_address < WIIMOTE_IR_CAMERA_FRAME_REGISTER +
                                      WIIMOTE_IR_CAMERA_FRAME_LENGTH) {
                output[i] = s_empty_ir_camera_frame[
                    register_address - WIIMOTE_IR_CAMERA_FRAME_REGISTER];
            }
        }
        return WIIMOTE_MEMORY_ERROR_SUCCESS;
    }

    if (slave != WIIMOTE_I2C_SLAVE_EXTENSION)
        return WIIMOTE_MEMORY_ERROR_NACK;
    if ((address == WIIMOTE_I2C_REG_EXTENSION_ENABLE ||
         address == WIIMOTE_I2C_REG_EXTENSION_ENABLE_2) && length == 1) {
        output[0] = 0x00;
        return WIIMOTE_MEMORY_ERROR_SUCCESS;
    }
    return WIIMOTE_MEMORY_ERROR_NACK;
}

static void memory_read_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            uint8_t space;
            uint8_t slave;
            uint8_t chunk;
            uint16_t address;

            portENTER_CRITICAL(&s_memory_mux);
            if (!s_memory_read.active || s_memory_read.remaining == 0) {
                s_memory_read.active = false;
                portEXIT_CRITICAL(&s_memory_mux);
                break;
            }
            space = s_memory_read.space;
            slave = s_memory_read.slave;
            address = s_memory_read.address;
            chunk = s_memory_read.remaining > WIIMOTE_MEMORY_REPORT_DATA_LEN ?
                        WIIMOTE_MEMORY_REPORT_DATA_LEN :
                        (uint8_t)s_memory_read.remaining;
            portEXIT_CRITICAL(&s_memory_mux);

            uint8_t bytes[WIIMOTE_MEMORY_REPORT_DATA_LEN] = {0};
            uint8_t error = read_wiimote_memory(space, slave, address, chunk, bytes);
            if (error != WIIMOTE_MEMORY_ERROR_SUCCESS) {
                wiimote_reports_send_memory_error(error, address);
                portENTER_CRITICAL(&s_memory_mux);
                s_memory_read.active = false;
                portEXIT_CRITICAL(&s_memory_mux);
                break;
            }

            wiimote_reports_send_memory_reply(error, address, bytes, chunk);
            portENTER_CRITICAL(&s_memory_mux);
            s_memory_read.address = (uint16_t)(s_memory_read.address + chunk);
            s_memory_read.remaining = (uint16_t)(s_memory_read.remaining - chunk);
            bool more = s_memory_read.remaining != 0;
            portEXIT_CRITICAL(&s_memory_mux);
            if (more) vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

esp_err_t wiimote_memory_start(void)
{
    if (s_memory_read_task != NULL) return ESP_OK;

    BaseType_t result = xTaskCreatePinnedToCore(memory_read_task, "memory_read", 3072,
                                    NULL, 5, &s_memory_read_task,
                                    NEXTMOTE_COMM_CORE);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create memory-read task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void wiimote_memory_reset(void)
{
    portENTER_CRITICAL(&s_memory_mux);
    s_memory_read.active = false;
    portEXIT_CRITICAL(&s_memory_mux);
}

uint8_t wiimote_memory_handle_read(const uint8_t *payload, uint16_t length)
{
    if (payload == NULL || length != 6)
        return ESP_HID_PAR_HANDSHAKE_RSP_ERR_INVALID_PARAM;

    uint8_t space = (payload[0] >> 2) & 0x03;
    uint8_t slave = payload[1] >> 1;
    uint16_t address = (uint16_t)((payload[2] << 8) | payload[3]);
    uint16_t requested = (uint16_t)((payload[4] << 8) | payload[5]);
    ESP_LOGI(TAG, "MEM_READ space=0x%02x slave=0x%02x address=0x%04x length=%u",
             space, slave, address, requested);
    if (requested == 0) return ESP_HID_PAR_HANDSHAKE_RSP_SUCCESS;

    portENTER_CRITICAL(&s_memory_mux);
    if (s_memory_read.active) {
        portEXIT_CRITICAL(&s_memory_mux);
        wiimote_reports_send_memory_error(WIIMOTE_MEMORY_ERROR_BUSY, address);
        return ESP_HID_PAR_HANDSHAKE_RSP_SUCCESS;
    }
    s_memory_read.active = true;
    s_memory_read.space = space;
    s_memory_read.slave = slave;
    s_memory_read.address = address;
    s_memory_read.remaining = requested;
    portEXIT_CRITICAL(&s_memory_mux);

    if (s_memory_read_task != NULL) {
        xTaskNotifyGive(s_memory_read_task);
    } else {
        portENTER_CRITICAL(&s_memory_mux);
        s_memory_read.active = false;
        portEXIT_CRITICAL(&s_memory_mux);
        ESP_LOGE(TAG, "Memory-read task is not running");
        return ESP_HID_PAR_HANDSHAKE_RSP_NOT_READY;
    }
    return ESP_HID_PAR_HANDSHAKE_RSP_SUCCESS;
}

uint8_t wiimote_memory_handle_write(const uint8_t *payload, uint16_t length)
{
    if (payload == NULL || length != 21)
        return ESP_HID_PAR_HANDSHAKE_RSP_ERR_INVALID_PARAM;

    uint8_t space = (payload[0] >> 2) & 0x03;
    uint8_t slave = payload[1] >> 1;
    uint16_t address = (uint16_t)((payload[2] << 8) | payload[3]);
    uint8_t count = payload[4];
    uint8_t error = WIIMOTE_MEMORY_ERROR_SUCCESS;

    if (count == 0 || count > 16) {
        error = WIIMOTE_MEMORY_ERROR_INVALID_ADDRESS;
    } else if (space != WIIMOTE_MEMORY_SPACE_EEPROM &&
               space != WIIMOTE_MEMORY_SPACE_I2C &&
               space != WIIMOTE_MEMORY_SPACE_I2C_ALT) {
        error = WIIMOTE_MEMORY_ERROR_INVALID_SPACE;
    } else if (space == WIIMOTE_MEMORY_SPACE_EEPROM ||
               slave == WIIMOTE_EEPROM_I2C_ADDRESS) {
        error = WIIMOTE_MEMORY_ERROR_INVALID_ADDRESS;
    } else if (slave == WIIMOTE_I2C_SLAVE_IR_CAMERA) {
        if ((uint32_t)address + count > sizeof(s_ir_camera_registers)) {
            error = WIIMOTE_MEMORY_ERROR_INVALID_ADDRESS;
        } else {
            portENTER_CRITICAL(&s_ir_camera_mux);
            memcpy(&s_ir_camera_registers[address], payload + 5, count);
            portEXIT_CRITICAL(&s_ir_camera_mux);
        }
    } else if (slave == WIIMOTE_I2C_SLAVE_EXTENSION &&
               address == WIIMOTE_I2C_REG_EXTENSION_ENABLE &&
               count == 1 && payload[5] == 0x55) {
        error = WIIMOTE_MEMORY_ERROR_SUCCESS;
    } else if (slave == WIIMOTE_I2C_SLAVE_EXTENSION &&
               address == WIIMOTE_I2C_REG_EXTENSION_ENABLE_2 &&
               count == 1 && payload[5] == 0x00) {
        error = WIIMOTE_MEMORY_ERROR_SUCCESS;
    } else {
        error = WIIMOTE_MEMORY_ERROR_NACK;
    }

    ESP_LOGI(TAG, "MEM_WRITE space=0x%02x slave=0x%02x address=0x%04x length=%u error=0x%02x",
             space, slave, address, count, error);
    wiimote_reports_send_memory_write_ack(error);
    return ESP_HID_PAR_HANDSHAKE_RSP_SUCCESS;
}

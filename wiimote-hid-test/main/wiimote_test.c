#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_err.h"
#include "esp_gap_bt_api.h"
#include "esp_wifiemote.h"
#include "esp_hidd_api.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "ir_camera.h"
#include "nvs_flash.h"
#include "wiimote_web.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define REPORT_STATUS 0x20
#define REPORT_READ_DATA_REPLY 0x21
#define REPORT_WRITE_DATA_ACK 0x22
#define REPORT_BUTTONS 0x30
#define REPORT_LED 0x11
#define REPORT_STATUS_REQUEST 0x15
#define REPORT_MODE 0x12
#define REPORT_ACK REPORT_WRITE_DATA_ACK
#define REPORT_READ_MEMORY 0x17
#define REPORT_WRITE_MEMORY 0x16
#define REPORT_IR_ENABLE 0x13
#define REPORT_IR_ENABLE_2 0x1a
#define REPORT_SPEAKER_ENABLE 0x14
#define REPORT_SPEAKER_MUTE 0x19
#define BUTTON_PAYLOAD_LEN 2
#define MEMORY_REPORT_DATA_LEN 16
#define MEMORY_REPORT_PAYLOAD_LEN 21
#define MEMORY_SPACE_EEPROM 0x00
#define MEMORY_SPACE_I2C 0x01
#define MEMORY_SPACE_I2C_ALT 0x02
#define MEMORY_ERROR_SUCCESS 0x00
#define MEMORY_ERROR_BUSY 0x04
#define MEMORY_ERROR_INVALID_SPACE 0x06
#define MEMORY_ERROR_NACK 0x07
#define MEMORY_ERROR_INVALID_ADDRESS 0x08
#define EEPROM_I2C_ADDRESS 0x50
#define I2C_SLAVE_EXTENSION 0x52
#define I2C_REG_EXTENSION_ENABLE 0xF0
#define I2C_REG_EXTENSION_ENABLE_2 0xFB
#define BUTTON_DEMO_PRESS_MS 500
#define BUTTON_DEMO_RELEASE_MS 250
#define BUTTON_DEMO_ACTIVE_REPORT 0x30
#define IR_CAMERA_POLL_MS 50

static const char *TAG = "espwiimote";
static const char local_device_name[] = CONFIG_EXAMPLE_LOCAL_DEVICE_NAME;

static struct {
    esp_hidd_app_param_t app;
    esp_hidd_qos_param_t qos;
    uint8_t buttons[BUTTON_PAYLOAD_LEN];
    uint8_t leds;
    uint8_t report_mode;
    bool continuous_reporting;
    bool connected;
} s_remote;

static uint8_t send_buttons(void);
static portMUX_TYPE s_remote_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_web_control_active;
static portMUX_TYPE s_memory_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    bool active;
    uint8_t space;
    uint8_t slave;
    uint16_t address;
    uint16_t remaining;
} s_memory_read;
static TaskHandle_t s_memory_read_task;

static void set_web_button_state(uint16_t state)
{
    portENTER_CRITICAL(&s_remote_mux);
    s_web_control_active = true;
    s_remote.buttons[0] = (uint8_t)(state >> 8);
    s_remote.buttons[1] = (uint8_t)(state & 0xff);
    bool send_now = s_remote.connected && s_remote.report_mode == REPORT_BUTTONS;
    portEXIT_CRITICAL(&s_remote_mux);
    if (send_now) (void)send_buttons();
}

#if CONFIG_EXAMPLE_WIFI_WEB_CONTROL
static void ir_camera_task(void *arg)
{
    (void)arg;
    ir_camera_state_t state = {0};
    esp_err_t last_error = ESP_OK;

    for (;;) {
        esp_err_t err = ir_camera_read(&state);
        if (err != ESP_OK && err != last_error) {
            ESP_LOGW(TAG, "IR camera frame read failed: %s", esp_err_to_name(err));
        } else if (err == ESP_OK && last_error != ESP_OK) {
            ESP_LOGI(TAG, "IR camera frame reads recovered");
        }
        last_error = err;
        wiimote_web_set_camera_state(&state);
        vTaskDelay(pdMS_TO_TICKS(IR_CAMERA_POLL_MS));
    }
}
#endif

#if CONFIG_EXAMPLE_FAKE_BUTTON_DEMO
static const struct {
    const char *name;
    uint16_t mask;
} s_button_sequence[] = {
    {"A", 0x0008}, {"B", 0x0004}, {"ONE", 0x0002}, {"TWO", 0x0001},
    {"UP", 0x0800}, {"DOWN", 0x0400}, {"LEFT", 0x0100}, {"RIGHT", 0x0200},
    {"PLUS", 0x1000}, {"MINUS", 0x0010}, {"HOME", 0x0080},
};

static void fake_button_task(void *arg)
{
    (void) arg;
    size_t index = 0;
    ESP_LOGI(TAG, "fake button demo ready; waiting for connected report mode 0x30");

    while (true) {
        bool demo_active;
        bool continuous_reporting;
        portENTER_CRITICAL(&s_remote_mux);
        demo_active = s_remote.connected &&
                      s_remote.report_mode == BUTTON_DEMO_ACTIVE_REPORT;
        continuous_reporting = s_remote.continuous_reporting;
        portEXIT_CRITICAL(&s_remote_mux);
        if (!demo_active || s_web_control_active) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        const uint16_t mask = s_button_sequence[index].mask;
        portENTER_CRITICAL(&s_remote_mux);
        s_remote.buttons[0] = (uint8_t) (mask & 0xff);
        s_remote.buttons[1] = (uint8_t) (mask >> 8);
        portEXIT_CRITICAL(&s_remote_mux);
        ESP_LOGI(TAG, "FAKE_BUTTON press %s (continuous=%d)",
                 s_button_sequence[index].name, continuous_reporting);
        (void) send_buttons();
        vTaskDelay(pdMS_TO_TICKS(BUTTON_DEMO_PRESS_MS));

        portENTER_CRITICAL(&s_remote_mux);
        s_remote.buttons[0] = 0;
        s_remote.buttons[1] = 0;
        portEXIT_CRITICAL(&s_remote_mux);
        ESP_LOGI(TAG, "FAKE_BUTTON release %s", s_button_sequence[index].name);
        if (continuous_reporting) (void) send_buttons();
        vTaskDelay(pdMS_TO_TICKS(BUTTON_DEMO_RELEASE_MS));

        index = (index + 1) % (sizeof(s_button_sequence) / sizeof(s_button_sequence[0]));
    }
}
#endif

static uint8_t send_input(uint8_t report_id, const uint8_t *data, uint16_t len)
{
    portENTER_CRITICAL(&s_remote_mux);
    bool connected = s_remote.connected;
    portEXIT_CRITICAL(&s_remote_mux);
    if (!connected) return ESP_HID_PAR_HANDSHAKE_RSP_SUCCESS;
    esp_err_t err = esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, report_id, len, (uint8_t *)data);
    ESP_LOGI(TAG, "tx id=0x%02x length=%u status=%s", report_id, len, esp_err_to_name(err));
    return err == ESP_OK ? ESP_HID_PAR_HANDSHAKE_RSP_SUCCESS : ESP_HID_PAR_HANDSHAKE_RSP_NOT_READY;
}

static uint8_t send_buttons(void)
{
    uint8_t buttons[BUTTON_PAYLOAD_LEN];
    portENTER_CRITICAL(&s_remote_mux);
    memcpy(buttons, s_remote.buttons, sizeof(buttons));
    portEXIT_CRITICAL(&s_remote_mux);
    return send_input(REPORT_BUTTONS, buttons, sizeof(buttons));
}

static uint8_t send_status(void)
{
    uint8_t payload[6];
    portENTER_CRITICAL(&s_remote_mux);
    payload[0] = s_remote.buttons[0];
    payload[1] = s_remote.buttons[1];
    payload[2] = s_remote.leds;
    portEXIT_CRITICAL(&s_remote_mux);
    payload[3] = 0;
    payload[4] = 0;
    payload[5] = 0xc0;
    return send_input(REPORT_STATUS, payload, sizeof(payload));
}

static void current_buttons(uint8_t buttons[BUTTON_PAYLOAD_LEN])
{
    portENTER_CRITICAL(&s_remote_mux);
    memcpy(buttons, s_remote.buttons, BUTTON_PAYLOAD_LEN);
    portEXIT_CRITICAL(&s_remote_mux);
}

static void send_interrupt_ack(uint8_t report_id, uint8_t error)
{
    uint8_t buttons[BUTTON_PAYLOAD_LEN];
    current_buttons(buttons);
    const uint8_t payload[4] = {buttons[0], buttons[1], report_id, error};
    (void) send_input(REPORT_ACK, payload, sizeof(payload));
}

static void send_memory_read_reply(uint8_t error, uint16_t address, const uint8_t *data, uint8_t length)
{
    uint8_t payload[MEMORY_REPORT_PAYLOAD_LEN] = {0};
    uint8_t buttons[BUTTON_PAYLOAD_LEN];
    current_buttons(buttons);
    payload[0] = buttons[0];
    payload[1] = buttons[1];
    payload[2] = (uint8_t)(((length ? ((length - 1) & 0x0f) : 0) << 4) | (error & 0x0f));
    payload[3] = (uint8_t)(address >> 8);
    payload[4] = (uint8_t)address;
    if (data && length) memcpy(payload + 5, data, length);
    ESP_LOGI(TAG, "MEM_READ_REPLY addr=0x%04x size=%u error=%u", address, length, error);
    ESP_LOG_BUFFER_HEX(TAG, payload, sizeof(payload));
    (void) send_input(REPORT_READ_DATA_REPLY, payload, sizeof(payload));
}

static void send_memory_read_error(uint8_t error, uint16_t address)
{
    uint8_t payload[MEMORY_REPORT_PAYLOAD_LEN] = {0};
    uint8_t buttons[BUTTON_PAYLOAD_LEN];
    current_buttons(buttons);
    payload[0] = buttons[0];
    payload[1] = buttons[1];
    payload[2] = (uint8_t)(0xf0 | (error & 0x0f));
    payload[3] = (uint8_t)(address >> 8);
    payload[4] = (uint8_t)address;
    ESP_LOGI(TAG, "MEM_READ_ERROR_REPLY addr=0x%04x error=%u size_minus_one=15",
             address, error);
    ESP_LOG_BUFFER_HEX(TAG, payload, sizeof(payload));
    (void) send_input(REPORT_READ_DATA_REPLY, payload, sizeof(payload));
}

static void send_memory_write_ack(uint8_t error)
{
    uint8_t buttons[BUTTON_PAYLOAD_LEN];
    current_buttons(buttons);
    const uint8_t payload[4] = {buttons[0], buttons[1], REPORT_WRITE_MEMORY, error};
    (void) send_input(REPORT_WRITE_DATA_ACK, payload, sizeof(payload));
}

static uint8_t read_wiimote_memory(uint8_t space, uint8_t slave, uint16_t address,
                                   uint8_t length, uint8_t *output)
{
    if (space == MEMORY_SPACE_EEPROM) {
        if (address == 0x0016 && length == 8) {
            static const uint8_t calibration[8] = {0x80, 0x80, 0x80, 0x00, 0x9a, 0x9a, 0x9a, 0x00};
            memcpy(output, calibration, sizeof(calibration));
            return MEMORY_ERROR_SUCCESS;
        }
        if (address >= 0x1700 || (uint32_t)address + length > 0x1700)
            return MEMORY_ERROR_INVALID_ADDRESS;
        memset(output, 0, length);
        return MEMORY_ERROR_SUCCESS;
    }
    if (space != MEMORY_SPACE_I2C && space != MEMORY_SPACE_I2C_ALT)
        return MEMORY_ERROR_INVALID_SPACE;
    if (slave == EEPROM_I2C_ADDRESS)
        return MEMORY_ERROR_INVALID_ADDRESS;
    if (slave != I2C_SLAVE_EXTENSION)
        return MEMORY_ERROR_NACK;

    if (address == I2C_REG_EXTENSION_ENABLE && length == 1) {
        output[0] = 0x00;
        return MEMORY_ERROR_SUCCESS;
    }
    if (address == I2C_REG_EXTENSION_ENABLE_2 && length == 1) {
        output[0] = 0x00;
        return MEMORY_ERROR_SUCCESS;
    }
    return MEMORY_ERROR_NACK;
}

static uint8_t handle_memory_read(const uint8_t *payload, uint16_t len)
{
    if (!payload || len != 6) return ESP_HID_PAR_HANDSHAKE_RSP_ERR_INVALID_PARAM;
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
        send_memory_read_error(MEMORY_ERROR_BUSY, address);
        return ESP_HID_PAR_HANDSHAKE_RSP_SUCCESS;
    }
    s_memory_read.active = true;
    s_memory_read.space = space;
    s_memory_read.slave = slave;
    s_memory_read.address = address;
    s_memory_read.remaining = requested;
    portEXIT_CRITICAL(&s_memory_mux);
    if (s_memory_read_task) xTaskNotifyGive(s_memory_read_task);
    return ESP_HID_PAR_HANDSHAKE_RSP_SUCCESS;
}

static void memory_read_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            uint8_t space, slave, chunk;
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
            chunk = s_memory_read.remaining > MEMORY_REPORT_DATA_LEN ?
                    MEMORY_REPORT_DATA_LEN : (uint8_t)s_memory_read.remaining;
            portEXIT_CRITICAL(&s_memory_mux);

            uint8_t bytes[MEMORY_REPORT_DATA_LEN] = {0};
            uint8_t error = read_wiimote_memory(space, slave, address, chunk, bytes);
            if (error != MEMORY_ERROR_SUCCESS) {
                send_memory_read_error(error, address);
                portENTER_CRITICAL(&s_memory_mux);
                s_memory_read.active = false;
                portEXIT_CRITICAL(&s_memory_mux);
                break;
            }
            send_memory_read_reply(error, address, bytes, chunk);
            portENTER_CRITICAL(&s_memory_mux);
            s_memory_read.address = (uint16_t)(s_memory_read.address + chunk);
            s_memory_read.remaining = (uint16_t)(s_memory_read.remaining - chunk);
            bool more = s_memory_read.remaining != 0;
            portEXIT_CRITICAL(&s_memory_mux);
            if (more) vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

static uint8_t handle_memory_write(const uint8_t *payload, uint16_t len)
{
    if (!payload || len != 21) return ESP_HID_PAR_HANDSHAKE_RSP_ERR_INVALID_PARAM;
    uint8_t space = (payload[0] >> 2) & 0x03;
    uint8_t slave = payload[1] >> 1;
    uint16_t address = (uint16_t)((payload[2] << 8) | payload[3]);
    uint8_t count = payload[4];
    ESP_LOGI(TAG, "MEM_WRITE space=0x%02x slave=0x%02x address=0x%04x length=%u",
             space, slave, address, count);
    uint8_t error = MEMORY_ERROR_SUCCESS;
    if (count == 0 || count > 16) {
        error = MEMORY_ERROR_INVALID_ADDRESS;
    } else if (space != MEMORY_SPACE_EEPROM && space != MEMORY_SPACE_I2C && space != MEMORY_SPACE_I2C_ALT) {
        error = MEMORY_ERROR_INVALID_SPACE;
    } else if (space == MEMORY_SPACE_EEPROM) {
        error = MEMORY_ERROR_INVALID_ADDRESS;
    } else if (slave == EEPROM_I2C_ADDRESS) {
        error = MEMORY_ERROR_INVALID_ADDRESS;
    } else if (space == MEMORY_SPACE_I2C || space == MEMORY_SPACE_I2C_ALT) {
        if (slave == I2C_SLAVE_EXTENSION && address == I2C_REG_EXTENSION_ENABLE &&
            count == 1 && payload[5] == 0x55) {
            error = MEMORY_ERROR_SUCCESS;
        } else if (slave == I2C_SLAVE_EXTENSION && address == I2C_REG_EXTENSION_ENABLE_2 &&
                   count == 1 && payload[5] == 0x00) {
            error = MEMORY_ERROR_SUCCESS;
        } else {
            error = MEMORY_ERROR_NACK;
        }
    }
    send_memory_write_ack(error);
    return ESP_HID_PAR_HANDSHAKE_RSP_SUCCESS;
}

static bool report_requests_ack(const uint8_t *payload, uint16_t length)
{
    return length > 0 && payload != NULL && (payload[0] & 0x02) != 0;
}

static uint8_t process_output_report(uint8_t report_id, const uint8_t *payload, uint16_t len)
{
    ESP_LOGI(TAG, "output id=0x%02x length=%u", report_id, len);
    if (len > 0 && payload != NULL) ESP_LOG_BUFFER_HEX(TAG, payload, len);

    switch (report_id) {
    case REPORT_LED:
        if (len != 1) return ESP_HID_PAR_HANDSHAKE_RSP_ERR_INVALID_PARAM;
        portENTER_CRITICAL(&s_remote_mux);
        s_remote.leds = payload[0] & 0xf0;
        portEXIT_CRITICAL(&s_remote_mux);
        ESP_LOGI(TAG, "LED state=0x%02x", s_remote.leds);
        return ESP_HID_PAR_HANDSHAKE_RSP_SUCCESS;
    case REPORT_IR_ENABLE:
    case REPORT_IR_ENABLE_2:
    case REPORT_SPEAKER_ENABLE:
    case REPORT_SPEAKER_MUTE:
        if (len != 1) return ESP_HID_PAR_HANDSHAKE_RSP_ERR_INVALID_PARAM;
        ESP_LOGI(TAG, "Feature output id=0x%02x enable=%u (sensor/audio hardware absent)",
                 report_id, (payload[0] & 0x04) != 0);
        return ESP_HID_PAR_HANDSHAKE_RSP_SUCCESS;
    case 0x10:
        if (len != 1) return ESP_HID_PAR_HANDSHAKE_RSP_ERR_INVALID_PARAM;
        ESP_LOGI(TAG, "Rumble state=%u (motor hardware absent)", payload[0] & 1);
        return ESP_HID_PAR_HANDSHAKE_RSP_SUCCESS;
    case REPORT_STATUS_REQUEST:
        if (len != 1) return ESP_HID_PAR_HANDSHAKE_RSP_ERR_INVALID_PARAM;
        (void) send_status();
        return ESP_HID_PAR_HANDSHAKE_RSP_SUCCESS;
    case REPORT_MODE:
        if (len != 2) return ESP_HID_PAR_HANDSHAKE_RSP_ERR_INVALID_PARAM;
        portENTER_CRITICAL(&s_remote_mux);
        s_remote.report_mode = payload[1];
        s_remote.continuous_reporting = (payload[0] & 0x04) != 0;
        bool continuous_reporting = s_remote.continuous_reporting;
        portEXIT_CRITICAL(&s_remote_mux);
        ESP_LOGI(TAG, "report mode=0x%02x flags=0x%02x (continuous=%d)", payload[1], payload[0],
                 continuous_reporting);
        return send_buttons();
    case REPORT_READ_MEMORY:
        return handle_memory_read(payload, len);
    case REPORT_WRITE_MEMORY:
        return handle_memory_write(payload, len);
    default:
        ESP_LOGW(TAG, "Unsupported Wii Remote output report 0x%02x", report_id);
        return ESP_HID_PAR_HANDSHAKE_RSP_ERR_UNSUPPORTED_REQ;
    }
}

/* A genuine RVL-CNT-01 advertises CoD 0x002504: peripheral/joystick plus the
   limited-discoverable service bit. esp_bt_gap_set_scan_mode() rewrites the
   service-class bits, so the CoD has to be re-applied after every mode change. */
static void apply_wiimote_cod(void)
{
    esp_bt_cod_t cod = {0};
    cod.major = ESP_BT_COD_MAJOR_DEV_PERIPHERAL;
    cod.minor = ESP_BT_COD_MINOR_PERIPHERAL_JOYSTICK;
#if CONFIG_EXAMPLE_LIMITED_DISCOVERABLE
    cod.service = ESP_BT_COD_SRVC_LMTD_DISCOVER;
#else
    cod.service = ESP_BT_COD_SRVC_NONE;
#endif
    ESP_ERROR_CHECK(esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_MAJOR_MINOR));
    ESP_ERROR_CHECK(esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_SERVICE_CLASS));

    esp_bt_cod_t readback = {0};
    esp_err_t err = esp_bt_gap_get_cod(&readback);
    if (err == ESP_OK) {
        unsigned value = ((unsigned) readback.service << 13) |
                         ((unsigned) readback.major << 8) |
                         ((unsigned) readback.minor << 2);
        ESP_LOGI(TAG, "CoD=0x%06x (reference RVL-CNT-01 is 0x002504)", value);
    } else {
        ESP_LOGW(TAG, "Could not read back CoD: %s", esp_err_to_name(err));
    }
}

static void enter_sync_discoverable(void)
{
#if CONFIG_EXAMPLE_LIMITED_DISCOVERABLE
    const esp_bt_discovery_mode_t mode = ESP_BT_LIMITED_DISCOVERABLE;
    const char *mode_name = "limited";
#else
    const esp_bt_discovery_mode_t mode = ESP_BT_GENERAL_DISCOVERABLE;
    const char *mode_name = "general";
#endif
    ESP_ERROR_CHECK(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, mode));
    apply_wiimote_cod();
    ESP_LOGI(TAG, "connectable, %s discoverable", mode_name);
}

static void on_gap_event(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        ESP_LOGI(TAG, "authentication status=%u peer=%s", param->auth_cmpl.stat, param->auth_cmpl.device_name);
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin = {0};
        ESP_LOGI(TAG, "legacy PIN request min16=%u", param->pin_req.min_16_digit);
        if (param->pin_req.min_16_digit) {
            esp_bd_addr_t local_address = {0};
            ESP_ERROR_CHECK(esp_read_mac(local_address, ESP_MAC_BT));
            for (size_t i = 0; i < ESP_BD_ADDR_LEN; ++i) pin[i] = local_address[ESP_BD_ADDR_LEN - 1 - i];
            ESP_LOGI(TAG, "Using local ESP32 BT address PIN for SYNC pairing");
        } else {
            for (size_t i = 0; i < ESP_BD_ADDR_LEN; ++i) pin[i] = param->pin_req.bda[ESP_BD_ADDR_LEN - 1 - i];
            ESP_LOGI(TAG, "Using peer BT address PIN for temporary pairing");
        }
        esp_err_t err = esp_bt_gap_pin_reply(param->pin_req.bda, true, ESP_BD_ADDR_LEN, pin);
        ESP_LOGI(TAG, "legacy PIN reply result=%s", esp_err_to_name(err));
        break;
    }
    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGW(TAG, "Unexpected Secure Simple Pairing request: this Wiimote test requires legacy PIN");
        break;
    case ESP_BT_GAP_MODE_CHG_EVT:
        ESP_LOGI(TAG, "mode change=%d", param->mode_chg.mode);
        break;
    default:
        ESP_LOGI(TAG, "GAP event=%d", event);
        break;
    }
}

static void on_hid_event(esp_hidd_cb_event_t event, esp_hidd_cb_param_t *param)
{
    switch (event) {
    case ESP_HIDD_INIT_EVT:
        ESP_LOGI(TAG, "HID initialized result=%d", param->init.status);
        if (param->init.status == ESP_HIDD_SUCCESS) {
            ESP_ERROR_CHECK(esp_bt_hid_device_register_app(&s_remote.app, &s_remote.qos, &s_remote.qos));
        }
        break;
    case ESP_HIDD_REGISTER_APP_EVT:
        ESP_LOGI(TAG, "HID SDP registration result=%d", param->register_app.status);
        if (param->register_app.status == ESP_HIDD_SUCCESS) {
            enter_sync_discoverable();
            ESP_LOGI(TAG, "READY identity=Nintendo RVL-CNT-01; awaiting external DolphinBar SYNC");
            if (param->register_app.in_use) {
                ESP_LOGI(TAG, "Stored host is present; no outgoing connection attempted");
            }
        }
        break;
    case ESP_HIDD_OPEN_EVT:
        ESP_LOGI(TAG, "HID open result=%d state=%d", param->open.status, param->open.conn_status);
        if (param->open.status == ESP_HIDD_SUCCESS && param->open.conn_status == ESP_HIDD_CONN_STATE_CONNECTED) {
            char peer[18];
            portENTER_CRITICAL(&s_remote_mux);
            s_remote.connected = true;
            s_web_control_active = false;
            s_remote.buttons[0] = 0;
            s_remote.buttons[1] = 0;
            portEXIT_CRITICAL(&s_remote_mux);
            s_memory_read.active = false;
            if (!s_memory_read_task) {
                BaseType_t task_result = xTaskCreate(memory_read_task, "memory_read", 3072, NULL, 5, &s_memory_read_task);
                ESP_ERROR_CHECK(task_result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
            }
            wiimote_web_set_hid_connected(true);
            ESP_LOGI(TAG, "WIIREMOTE_HID_CONNECTED peer=%s", wiimote_bt_address_string(param->open.bd_addr, peer));
            ESP_ERROR_CHECK(esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE));
            s_remote.connected = true;
            (void) send_status();
            (void) send_buttons();
        }
        break;
    case ESP_HIDD_CLOSE_EVT:
        ESP_LOGI(TAG, "HID close result=%d state=%d", param->close.status, param->close.conn_status);
        if (param->close.conn_status == ESP_HIDD_CONN_STATE_DISCONNECTED) {
            portENTER_CRITICAL(&s_remote_mux);
            s_remote.connected = false;
            s_remote.buttons[0] = 0;
            s_remote.buttons[1] = 0;
            portEXIT_CRITICAL(&s_remote_mux);
            wiimote_web_set_hid_connected(false);
            enter_sync_discoverable();
        }
        break;
    case ESP_HIDD_GET_REPORT_EVT:
        ESP_LOGI(TAG, "GET_REPORT id=0x%02x type=%u", param->get_report.report_id, param->get_report.report_type);
        if (param->get_report.report_type == ESP_HIDD_REPORT_TYPE_INPUT && param->get_report.report_id == REPORT_BUTTONS) {
            (void) send_buttons();
        } else {
            (void) esp_bt_hid_device_report_error(ESP_HID_PAR_HANDSHAKE_RSP_ERR_INVALID_REP_ID);
        }
        break;
    case ESP_HIDD_SET_REPORT_EVT: {
        uint8_t error = process_output_report(param->set_report.report_id, param->set_report.data,
                                              param->set_report.len);
        bool ack_requested = report_requests_ack(param->set_report.data, param->set_report.len);
        if (param->set_report.report_type == ESP_HIDD_REPORT_TYPE_OUTPUT) {
            esp_err_t result = esp_bt_hid_device_report_error((esp_hidd_handshake_error_t) error);
            ESP_LOGI(TAG, "SET_REPORT id=0x%02x handshake=0x%02x result=%s",
                     param->set_report.report_id, error, esp_err_to_name(result));
        } else if (ack_requested) {
            send_interrupt_ack(param->set_report.report_id, error);
        }
        break;
    }
    case ESP_HIDD_INTR_DATA_EVT: {
        uint8_t report_id = param->intr_data.report_id;
        uint8_t *data = param->intr_data.data;
        uint16_t len = param->intr_data.len;
        ESP_LOGI(TAG, "interrupt data id=0x%02x length=%u", report_id, len);
        if (len > 0 && data != NULL) ESP_LOG_BUFFER_HEX(TAG, data, len);
        if (report_id == 0 && len > 1) {
            report_id = data[0];
            ++data;
            --len;
        }
        if (report_id == REPORT_ACK) {
            ESP_LOGI(TAG, "Host acknowledgement received");
        } else {
            uint8_t result = process_output_report(report_id, data, len);
            bool ack_requested = report_requests_ack(data, len);
            if (ack_requested) send_interrupt_ack(report_id, result);
        }
        break;
    }
    case ESP_HIDD_SET_PROTOCOL_EVT:
        ESP_LOGI(TAG, "HID protocol mode=%u (only report mode supported)", param->set_protocol.protocol_mode);
        break;
    case ESP_HIDD_REPORT_ERR_EVT:
        ESP_LOGI(TAG, "HID handshake result=%d reason=%u", param->report_err.status, param->report_err.reason);
        break;
    case ESP_HIDD_VC_UNPLUG_EVT:
        ESP_LOGI(TAG, "virtual cable unplug result=%d state=%d", param->vc_unplug.status, param->vc_unplug.conn_status);
        if (param->vc_unplug.conn_status == ESP_HIDD_CONN_STATE_DISCONNECTED) {
            portENTER_CRITICAL(&s_remote_mux);
            s_remote.connected = false;
            s_remote.buttons[0] = 0;
            s_remote.buttons[1] = 0;
            portEXIT_CRITICAL(&s_remote_mux);
            wiimote_web_set_hid_connected(false);
            enter_sync_discoverable();
        }
        break;
    default:
        ESP_LOGI(TAG, "HID event=%d", event);
        break;
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t controller_config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&controller_config));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));

    esp_bluedroid_config_t host_config = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    host_config.ssp_en = false;
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&host_config));
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    ESP_ERROR_CHECK(esp_bt_gap_register_callback(on_gap_event));
    ESP_ERROR_CHECK(esp_bt_hid_device_register_callback(on_hid_event));
    ESP_ERROR_CHECK(esp_bt_gap_set_device_name(local_device_name));

    apply_wiimote_cod();

    s_remote.app.name = "Nintendo RVL-CNT-01";
    s_remote.app.description = "Nintendo RVL-CNT-01";
    s_remote.app.provider = "Nintendo";
    s_remote.app.subclass = ESP_HID_CLASS_GPD;
    s_remote.app.desc_list = wiimote_report_descriptor;
    s_remote.app.desc_list_len = wiimote_report_descriptor_len;
    memset(&s_remote.qos, 0, sizeof(s_remote.qos));

    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
    esp_bt_pin_code_t empty_pin = {0};
    ESP_ERROR_CHECK(esp_bt_gap_set_pin(pin_type, 0, empty_pin));

    esp_bd_addr_t local_address;
    ESP_ERROR_CHECK(esp_read_mac(local_address, ESP_MAC_BT));
    ESP_LOGI(TAG, "ESP32 WROOM Classic Bluetooth HID peripheral address=%02x:%02x:%02x:%02x:%02x:%02x",
             local_address[0], local_address[1], local_address[2], local_address[3], local_address[4], local_address[5]);

    ESP_ERROR_CHECK(esp_bt_hid_device_init());
#if CONFIG_EXAMPLE_WIFI_WEB_CONTROL
    wiimote_web_set_button_callback(set_web_button_state);
    ir_camera_state_t camera_state = {0};
    esp_err_t camera_err = ir_camera_init();
    if (camera_err == ESP_OK) {
        camera_state.detected = true;
        wiimote_web_set_camera_state(&camera_state);
        BaseType_t task_result = xTaskCreate(ir_camera_task, "ir_camera", 3072, NULL, 5, NULL);
        if (task_result != pdPASS) {
            ESP_LOGE(TAG, "Failed to create IR camera polling task");
        }
    } else {
        ESP_LOGW(TAG, "IR camera initialization at 0x58 failed: %s; continuing without camera",
                 esp_err_to_name(camera_err));
        wiimote_web_set_camera_state(&camera_state);
    }
    wiimote_web_start();
#endif
#if CONFIG_EXAMPLE_FAKE_BUTTON_DEMO
    BaseType_t task_result = xTaskCreate(fake_button_task, "fake_button_demo", 4096, NULL, 5, NULL);
    ESP_ERROR_CHECK(task_result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
#endif
}

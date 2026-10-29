#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_err.h"
#include "esp_gap_bt_api.h"
#include "esp_hidd_api.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifiemote.h"
#include "nvs_flash.h"
#include "wiimote_commands.h"
#include "wiimote_controls.h"
#include "wiimote_memory.h"
#include "wiimote_protocol.h"
#include "wiimote_reports.h"
#include "wiimote_state.h"
#include "wiimote_web.h"
#include "ir_camera_service.h"
#include "imu_service.h"

static const char *TAG = "espwiimote";
static const char local_device_name[] = CONFIG_EXAMPLE_LOCAL_DEVICE_NAME;
static esp_hidd_app_param_t s_app;
static esp_hidd_qos_param_t s_qos;

/* Preserve the Wii Remote's joystick/peripheral class through discoverability changes. */
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
        unsigned value = ((unsigned)readback.service << 13) |
                         ((unsigned)readback.major << 8) |
                         ((unsigned)readback.minor << 2);
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
        ESP_LOGI(TAG, "authentication status=%u peer=%s",
                 param->auth_cmpl.stat, param->auth_cmpl.device_name);
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin = {0};
        ESP_LOGI(TAG, "legacy PIN request min16=%u", param->pin_req.min_16_digit);
        if (param->pin_req.min_16_digit) {
            esp_bd_addr_t local_address = {0};
            ESP_ERROR_CHECK(esp_read_mac(local_address, ESP_MAC_BT));
            for (size_t i = 0; i < ESP_BD_ADDR_LEN; ++i) {
                pin[i] = local_address[ESP_BD_ADDR_LEN - 1 - i];
            }
            ESP_LOGI(TAG, "Using local ESP32 BT address PIN for SYNC pairing");
        } else {
            for (size_t i = 0; i < ESP_BD_ADDR_LEN; ++i) {
                pin[i] = param->pin_req.bda[ESP_BD_ADDR_LEN - 1 - i];
            }
            ESP_LOGI(TAG, "Using peer BT address PIN for temporary pairing");
        }
        esp_err_t err = esp_bt_gap_pin_reply(param->pin_req.bda, true,
                                             ESP_BD_ADDR_LEN, pin);
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
            ESP_ERROR_CHECK(esp_bt_hid_device_register_app(&s_app, &s_qos, &s_qos));
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
        ESP_LOGI(TAG, "HID open result=%d state=%d",
                 param->open.status, param->open.conn_status);
        if (param->open.status == ESP_HIDD_SUCCESS &&
            param->open.conn_status == ESP_HIDD_CONN_STATE_CONNECTED) {
            char peer[18];
            wiimote_state_open();
            wiimote_controls_on_connection_open();
            wiimote_memory_reset();
            ESP_ERROR_CHECK(wiimote_memory_start());
            wiimote_web_set_hid_connected(true);
            ESP_LOGI(TAG, "WIIREMOTE_HID_CONNECTED peer=%s",
                     wiimote_bt_address_string(param->open.bd_addr, peer));
            ESP_ERROR_CHECK(esp_bt_gap_set_scan_mode(
                ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE));
            (void)wiimote_reports_send_status();
            (void)wiimote_reports_send_current();
        }
        break;
    case ESP_HIDD_CLOSE_EVT:
        ESP_LOGI(TAG, "HID close result=%d state=%d",
                 param->close.status, param->close.conn_status);
        wiimote_state_close();
        wiimote_web_set_hid_connected(false);
        enter_sync_discoverable();
        break;
    case ESP_HIDD_GET_REPORT_EVT:
        ESP_LOGI(TAG, "GET_REPORT id=0x%02x type=%u",
                 param->get_report.report_id, param->get_report.report_type);
        if (param->get_report.report_type == ESP_HIDD_REPORT_TYPE_INPUT &&
            wiimote_protocol_is_data_report(param->get_report.report_id)) {
            (void)wiimote_reports_send_data(param->get_report.report_id);
        } else {
            (void)esp_bt_hid_device_report_error(
                ESP_HID_PAR_HANDSHAKE_RSP_ERR_INVALID_REP_ID);
        }
        break;
    case ESP_HIDD_SET_REPORT_EVT: {
        uint8_t error = wiimote_commands_process_output_report(
            param->set_report.report_id, param->set_report.data,
            param->set_report.len);
        bool ack_requested = wiimote_protocol_report_requests_ack(
            param->set_report.data, param->set_report.len);
        if (param->set_report.report_type == ESP_HIDD_REPORT_TYPE_OUTPUT) {
            esp_err_t result = esp_bt_hid_device_report_error(
                (esp_hidd_handshake_error_t)error);
            ESP_LOGI(TAG, "SET_REPORT id=0x%02x handshake=0x%02x result=%s",
                     param->set_report.report_id, error, esp_err_to_name(result));
        } else if (ack_requested) {
            wiimote_reports_send_interrupt_ack(param->set_report.report_id, error);
        }
        break;
    }
    case ESP_HIDD_INTR_DATA_EVT: {
        uint8_t report_id = param->intr_data.report_id;
        uint8_t *data = param->intr_data.data;
        uint16_t length = param->intr_data.len;
        ESP_LOGI(TAG, "interrupt data id=0x%02x length=%u", report_id, length);
        if (length > 0 && data != NULL) ESP_LOG_BUFFER_HEX(TAG, data, length);
        if (report_id == 0 && length > 1 && data != NULL) {
            report_id = data[0];
            ++data;
            --length;
        }
        if (report_id == WIIMOTE_REPORT_ACK) {
            ESP_LOGI(TAG, "Host acknowledgement received");
        } else {
            uint8_t result = wiimote_commands_process_output_report(
                report_id, data, length);
            bool ack_requested = wiimote_protocol_report_requests_ack(data, length);
            if (ack_requested) {
                wiimote_reports_send_interrupt_ack(report_id, result);
            }
        }
        break;
    }
    case ESP_HIDD_SET_PROTOCOL_EVT:
        ESP_LOGI(TAG, "HID protocol mode=%u (only report mode supported)",
                 param->set_protocol.protocol_mode);
        break;
    case ESP_HIDD_REPORT_ERR_EVT:
        ESP_LOGI(TAG, "HID handshake result=%d reason=%u",
                 param->report_err.status, param->report_err.reason);
        break;
    case ESP_HIDD_VC_UNPLUG_EVT:
        ESP_LOGI(TAG, "virtual cable unplug result=%d state=%d",
                 param->vc_unplug.status, param->vc_unplug.conn_status);
        if (param->vc_unplug.conn_status == ESP_HIDD_CONN_STATE_DISCONNECTED) {
            wiimote_state_close();
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

    s_app.name = "Nintendo RVL-CNT-01";
    s_app.description = "Nintendo RVL-CNT-01";
    s_app.provider = "Nintendo";
    s_app.subclass = ESP_HID_CLASS_GPD;
    s_app.desc_list = wiimote_report_descriptor;
    s_app.desc_list_len = wiimote_report_descriptor_len;
    memset(&s_qos, 0, sizeof(s_qos));

    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
    esp_bt_pin_code_t empty_pin = {0};
    ESP_ERROR_CHECK(esp_bt_gap_set_pin(pin_type, 0, empty_pin));

    esp_bd_addr_t local_address;
    ESP_ERROR_CHECK(esp_read_mac(local_address, ESP_MAC_BT));
    ESP_LOGI(TAG, "ESP32 WROOM Classic Bluetooth HID peripheral address=%02x:%02x:%02x:%02x:%02x:%02x",
             local_address[0], local_address[1], local_address[2],
             local_address[3], local_address[4], local_address[5]);

    ESP_ERROR_CHECK(esp_bt_hid_device_init());
#if CONFIG_EXAMPLE_WIFI_WEB_CONTROL
    wiimote_controls_init();
    ir_camera_service_start();
    wiimote_web_start();
#endif
    esp_err_t imu_err = imu_service_start();
    if (imu_err != ESP_OK) {
        ESP_LOGW(TAG, "BNO08x orientation service unavailable: %s",
                 esp_err_to_name(imu_err));
    }
    ESP_ERROR_CHECK(wiimote_controls_start_fake_demo());
}

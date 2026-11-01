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
#include "esp_sdp_api.h"
#include "esp_timer.h"
#include "esp_wifiemote.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "nvs_flash.h"
#include "wiimote_commands.h"
#include "wiimote_controls.h"
#include "wiimote_discovery.h"
#include "wiimote_memory.h"
#include "wiimote_protocol.h"
#include "wiimote_reports.h"
#include "wiimote_sdp.h"
#include "wiimote_sdp_values.h"
#include "wiimote_state.h"
#include "wiimote_web.h"
#include "ir_camera_service.h"
#include "imu_service.h"
#if CONFIG_EXAMPLE_BT_DISCOVERY_TRACE
#include "wiimote_bt_trace.h"
#endif

static const char *TAG = "espwiimote";
static const char local_device_name[] = CONFIG_EXAMPLE_LOCAL_DEVICE_NAME;
static esp_hidd_app_param_t s_app;
static esp_hidd_qos_param_t s_qos;
static esp_timer_handle_t s_sync_pairing_timer;
static portMUX_TYPE s_sync_pairing_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_sync_pairing_active;
static portMUX_TYPE s_device_id_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_device_id_record_ready;
static esp_bluetooth_sdp_record_t s_device_id_record;
#if WIIMOTE_UPSTREAM_DISCOVERY_BASELINE
static void maybe_arm_upstream_discovery(const char *reason);
#endif

/* Match the explicit startup CoD in the upstream real-Wii revision. */
static esp_err_t apply_wiimote_cod(void)
{
    esp_bt_cod_t cod = {0};
    cod.major = ESP_BT_COD_MAJOR_DEV_PERIPHERAL;
    cod.minor = ESP_BT_COD_MINOR_PERIPHERAL_JOYSTICK;
    cod.service = ESP_BT_COD_SRVC_LMTD_DISCOVER;
    esp_err_t err = esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_ALL);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "wiimote startup CoD update queued: major/minor/service");
    }
    return err;
}

static esp_err_t set_pairing_discoverability(bool enabled, bool general_discoverable)
{
#if WIIMOTE_UPSTREAM_DISCOVERY_BASELINE
    (void)general_discoverable;
    esp_err_t err = esp_bt_gap_set_scan_mode(
        enabled ? ESP_BT_CONNECTABLE : ESP_BT_NON_CONNECTABLE,
        enabled ? ESP_BT_GENERAL_DISCOVERABLE : ESP_BT_NON_DISCOVERABLE);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Upstream General discovery %s requested",
                 enabled ? "active" : "hidden");
    }
    return err;
#else
    esp_bt_connection_mode_t connection_mode =
        enabled ? ESP_BT_CONNECTABLE : ESP_BT_NON_CONNECTABLE;
    esp_bt_discovery_mode_t discovery_mode = ESP_BT_NON_DISCOVERABLE;
    if (enabled) {
        discovery_mode = general_discoverable
                             ? ESP_BT_GENERAL_DISCOVERABLE : ESP_BT_LIMITED_DISCOVERABLE;
    }

    esp_err_t err = wiimote_discovery_set_rotation(false);
    if (err != ESP_OK && enabled) return err;
    if (enabled) {
        esp_err_t reset_err = esp_bt_gap_set_scan_mode(
            ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
        if (reset_err != ESP_OK) return reset_err;
    }
    esp_err_t scan_err = esp_bt_gap_set_scan_mode(connection_mode, discovery_mode);
    if (scan_err != ESP_OK) return scan_err;
    if (err != ESP_OK) return err;
    err = wiimote_discovery_set_rotation(enabled && !general_discoverable);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "Bluetooth pairing %s requested; connectable=%u discoverable=%u",
             enabled ? "window open" : "hidden",
             (unsigned)connection_mode, (unsigned)discovery_mode);
    return ESP_OK;
#endif
}

static bool sync_pairing_window_is_active(void)
{
    portENTER_CRITICAL(&s_sync_pairing_mux);
    bool active = s_sync_pairing_active;
    portEXIT_CRITICAL(&s_sync_pairing_mux);
    return active;
}

static void set_sync_pairing_window_active(bool active)
{
    portENTER_CRITICAL(&s_sync_pairing_mux);
    s_sync_pairing_active = active;
    portEXIT_CRITICAL(&s_sync_pairing_mux);
    wiimote_web_set_sync_window(active);
}

static void stop_sync_pairing_timer(void)
{
    if (!s_sync_pairing_timer || !esp_timer_is_active(s_sync_pairing_timer)) return;

    esp_err_t err = esp_timer_stop(s_sync_pairing_timer);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Could not stop SYNC timer: %s", esp_err_to_name(err));
    }
}

#if CONFIG_BT_SDP_COMMON_ENABLED
static bool device_id_record_is_ready(void)
{
    portENTER_CRITICAL(&s_device_id_mux);
    bool ready = s_device_id_record_ready;
    portEXIT_CRITICAL(&s_device_id_mux);
    return ready;
}

static void set_device_id_record_ready(bool ready)
{
    portENTER_CRITICAL(&s_device_id_mux);
    s_device_id_record_ready = ready;
    portEXIT_CRITICAL(&s_device_id_mux);
}

static void on_sdp_event(esp_sdp_cb_event_t event, esp_sdp_cb_param_t *param)
{
    switch (event) {
    case ESP_SDP_INIT_EVT: {
        if (param->init.status != ESP_SDP_SUCCESS) {
            ESP_LOGE(TAG, "SDP initialization failed: status=%d", param->init.status);
            break;
        }

        memset(&s_device_id_record, 0, sizeof(s_device_id_record));
        s_device_id_record.dip.hdr.type = ESP_SDP_TYPE_DIP_SERVER;
        s_device_id_record.dip.vendor = 0x057e;
        s_device_id_record.dip.vendor_id_source = ESP_SDP_VENDOR_ID_SRC_USB;
        s_device_id_record.dip.product = 0x0306;
        s_device_id_record.dip.version = WIIMOTE_DEVICE_ID_VERSION;
        s_device_id_record.dip.primary_record = true;
        esp_err_t err = esp_sdp_create_record(&s_device_id_record);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not create Nintendo Device ID SDP record: %s",
                     esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "Nintendo Device ID SDP record creation requested: VID=057e PID=0306");
        }
        break;
    }
    case ESP_SDP_CREATE_RECORD_COMP_EVT: {
        if (param->create_record.status == ESP_SDP_SUCCESS) {
            if (param->create_record.record_handle <= 0) {
                ESP_LOGE(TAG, "Invalid Nintendo Device ID SDP record handle");
                break;
            }
            esp_err_t err = wiimote_sdp_apply_device_id(
                (uint32_t)param->create_record.record_handle);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Could not apply wiimote Device ID SDP policy: %s",
                         esp_err_to_name(err));
                esp_err_t remove_err = esp_sdp_remove_record(param->create_record.record_handle);
                if (remove_err != ESP_OK) {
                    ESP_LOGE(TAG, "Could not remove incomplete Device ID record: %s",
                             esp_err_to_name(remove_err));
                }
                break;
            }
            set_device_id_record_ready(true);
            ESP_LOGI(TAG, "Nintendo Device ID SDP record ready: handle=0x%x",
                     param->create_record.record_handle);
#if WIIMOTE_UPSTREAM_DISCOVERY_BASELINE
            maybe_arm_upstream_discovery("Device ID ready");
#endif
        } else {
            ESP_LOGE(TAG, "Nintendo Device ID SDP record creation failed: status=%d",
                     param->create_record.status);
        }
        break;
    }
    default:
        break;
    }
}
#endif

static void sync_pairing_timeout(void *arg)
{
    (void)arg;
    if (!sync_pairing_window_is_active()) return;

    set_sync_pairing_window_active(false);
    if (wiimote_state_is_connected()) return;

    esp_err_t err = set_pairing_discoverability(false, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not close expired SYNC window: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "SYNC pairing window expired");
}

static void close_sync_pairing_window(const char *reason)
{
    bool was_active = sync_pairing_window_is_active();
    set_sync_pairing_window_active(false);
    stop_sync_pairing_timer();

    esp_err_t err = set_pairing_discoverability(false, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not close SYNC pairing window: %s", esp_err_to_name(err));
    } else if (was_active) {
        ESP_LOGI(TAG, "SYNC pairing window closed: %s", reason);
    }
}

static void discovery_error(esp_err_t error)
{
    ESP_LOGE(TAG, "Closing SYNC after discovery failure: %s", esp_err_to_name(error));
    close_sync_pairing_window("inquiry rotation failed");
}

static esp_err_t start_sync_pairing_window(bool general_discoverable)
{
    if (wiimote_state_is_connected() ||
        (!WIIMOTE_UPSTREAM_DISCOVERY_BASELINE && !s_sync_pairing_timer)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!wiimote_sdp_hid_record_is_ready()) {
        ESP_LOGW(TAG, "Cannot start SYNC before the wiimote HID SDP record is ready");
        return ESP_ERR_INVALID_STATE;
    }
#if CONFIG_BT_SDP_COMMON_ENABLED
    if (!device_id_record_is_ready()) {
        ESP_LOGW(TAG, "Cannot start SYNC pairing before the Nintendo Device ID SDP record is ready");
        return ESP_ERR_INVALID_STATE;
    }
#endif
#if WIIMOTE_UPSTREAM_DISCOVERY_BASELINE
    if (sync_pairing_window_is_active()) return ESP_OK;
    general_discoverable = true;
#endif

    stop_sync_pairing_timer();
    set_sync_pairing_window_active(true);

    esp_err_t err = set_pairing_discoverability(true, general_discoverable);
    if (err != ESP_OK) {
        set_sync_pairing_window_active(false);
        esp_err_t close_err = set_pairing_discoverability(false, false);
        if (close_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to clean up incomplete SYNC window: %s",
                     esp_err_to_name(close_err));
        }
        return err;
    }

#if WIIMOTE_UPSTREAM_DISCOVERY_BASELINE
    ESP_LOGI(TAG, "UPSTREAM_GENERAL_DISCOVERY_OPEN pairing=bonded duration=continuous");
    return ESP_OK;
#else
    err = esp_timer_start_once(s_sync_pairing_timer,
                               (uint64_t)WIIMOTE_SYNC_WINDOW_SECONDS * 1000000ULL);
    if (err != ESP_OK) {
        set_sync_pairing_window_active(false);
        esp_err_t close_err = set_pairing_discoverability(false, false);
        if (close_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to clean up SYNC window after timer error: %s",
                     esp_err_to_name(close_err));
        }
        return err;
    }

    ESP_LOGI(TAG,
             "SYNC_PAIRING_WINDOW_OPEN seconds=%u pairing=bonded discovery=%s inquiry_window=0x%04x inquiry_interval=0x%04x page_window=0x%04x page_interval=0x%04x",
             (unsigned)WIIMOTE_SYNC_WINDOW_SECONDS,
             general_discoverable ? "general-diagnostic" : "limited",
             (unsigned)WIIMOTE_BT_INQUIRY_SCAN_WINDOW,
             (unsigned)WIIMOTE_BT_INQUIRY_SCAN_INTERVAL,
             (unsigned)WIIMOTE_BT_PAGE_SCAN_WINDOW,
             (unsigned)WIIMOTE_BT_PAGE_SCAN_INTERVAL);
    return ESP_OK;
#endif
}

#if WIIMOTE_UPSTREAM_DISCOVERY_BASELINE
static void maybe_arm_upstream_discovery(const char *reason)
{
    if (wiimote_state_is_connected() || sync_pairing_window_is_active() ||
        !wiimote_sdp_hid_record_is_ready()) return;
#if CONFIG_BT_SDP_COMMON_ENABLED
    if (!device_id_record_is_ready()) return;
#endif
    esp_err_t err = start_sync_pairing_window(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not arm upstream discovery after %s: %s",
                 reason, esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG, "Upstream discovery baseline active after %s; General while idle, no timed timeout",
                 reason);
    }
}
#endif

static void on_gap_event(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        ESP_LOGI(TAG, "authentication status=%u peer=%s",
                 param->auth_cmpl.stat, param->auth_cmpl.device_name);
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS &&
            !WIIMOTE_UPSTREAM_DISCOVERY_BASELINE) {
            close_sync_pairing_window("authentication complete");
        }
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin = {0};
        ESP_LOGI(TAG, "legacy PIN request min16=%u", param->pin_req.min_16_digit);
        if (!sync_pairing_window_is_active()) {
            ESP_LOGW(TAG, "Rejecting PIN request outside the web SYNC pairing window");
            esp_err_t err = esp_bt_gap_pin_reply(param->pin_req.bda, false, 0, pin);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "PIN request rejection failed: %s", esp_err_to_name(err));
            }
            break;
        }
        if (param->pin_req.min_16_digit) {
            ESP_LOGE(TAG, "Rejecting PIN request requiring 16 digits; Wii Remote SYNC uses a 6-byte address PIN");
            esp_err_t err = esp_bt_gap_pin_reply(param->pin_req.bda, false, 0, pin);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "PIN request rejection failed: %s", esp_err_to_name(err));
            }
            break;
        }
        for (size_t i = 0; i < ESP_BD_ADDR_LEN; ++i) {
            pin[i] = param->pin_req.bda[ESP_BD_ADDR_LEN - 1 - i];
        }
        esp_err_t err = esp_bt_gap_pin_reply(param->pin_req.bda, true,
                                             ESP_BD_ADDR_LEN, pin);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "legacy PIN reply result=%s mode=SYNC pin=reverse-host-address",
                     esp_err_to_name(err));
        } else {
            ESP_LOGE(TAG, "legacy PIN reply failed: %s", esp_err_to_name(err));
        }
        break;
    }
    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGW(TAG, "Unexpected Secure Simple Pairing request: this Wiimote test requires legacy PIN");
        break;
    case ESP_BT_GAP_MODE_CHG_EVT:
        ESP_LOGI(TAG, "mode change=%d", param->mode_chg.mode);
        break;
    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT: {
        char peer[18];
        ESP_LOGI(TAG, "ACL connection status=%u handle=0x%04x peer=%s",
                 (unsigned)param->acl_conn_cmpl_stat.stat,
                 (unsigned)param->acl_conn_cmpl_stat.handle,
                 wiimote_bt_address_string(param->acl_conn_cmpl_stat.bda, peer));
        break;
    }
    case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT: {
        char peer[18];
        ESP_LOGI(TAG, "ACL disconnected reason=%u handle=0x%04x peer=%s",
                 (unsigned)param->acl_disconn_cmpl_stat.reason,
                 (unsigned)param->acl_disconn_cmpl_stat.handle,
                 wiimote_bt_address_string(param->acl_disconn_cmpl_stat.bda, peer));
        break;
    }
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
            if (!wiimote_sdp_hid_record_is_ready()) {
                ESP_LOGE(TAG, "HID registration did not produce a complete wiimote SDP record");
                esp_err_t err = esp_bt_hid_device_unregister_app();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Could not unregister incomplete HID app: %s", esp_err_to_name(err));
                }
                break;
            }
#if WIIMOTE_UPSTREAM_DISCOVERY_BASELINE
            maybe_arm_upstream_discovery("HID app ready");
#elif CONFIG_EXAMPLE_WIFI_WEB_CONTROL
            esp_err_t err = set_pairing_discoverability(false, false);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Could not disable startup discoverability: %s",
                         esp_err_to_name(err));
            }
            ESP_LOGI(TAG, "READY identity=Nintendo RVL-CNT-01; use web SYNC to open a 30-second pairing window");
#else
#if CONFIG_EXAMPLE_LIMITED_DISCOVERABLE
            esp_err_t err = set_pairing_discoverability(true, false);
#else
            esp_err_t err = set_pairing_discoverability(true, true);
#endif
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Could not enable startup discoverability: %s",
                         esp_err_to_name(err));
            }
            ESP_LOGI(TAG, "READY identity=Nintendo RVL-CNT-01; discoverable without web control");
#endif
            if (param->register_app.in_use) {
                ESP_LOGI(TAG, "HID app is already in use; no outgoing connection attempted");
            }
        }
        break;
    case ESP_HIDD_UNREGISTER_APP_EVT:
        wiimote_sdp_clear_hid_ready();
        ESP_LOGI(TAG, "HID app unregistered result=%d", param->unregister_app.status);
        break;
    case ESP_HIDD_DEINIT_EVT:
        wiimote_sdp_clear_hid_ready();
        ESP_LOGI(TAG, "HID deinitialized result=%d", param->deinit.status);
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
            close_sync_pairing_window("HID connection opened");
            ESP_LOGI(TAG, "WIIREMOTE_HID_CONNECTED peer=%s",
                     wiimote_bt_address_string(param->open.bd_addr, peer));
            (void)wiimote_reports_send_current();
        }
#if WIIMOTE_UPSTREAM_DISCOVERY_BASELINE
        else if (param->open.status != ESP_HIDD_SUCCESS ||
                 param->open.conn_status == ESP_HIDD_CONN_STATE_DISCONNECTED) {
            maybe_arm_upstream_discovery("HID open failed");
        }
#endif
        break;
    case ESP_HIDD_CLOSE_EVT:
        ESP_LOGI(TAG, "HID close result=%d state=%d",
                 param->close.status, param->close.conn_status);
        if (param->close.status != ESP_HIDD_SUCCESS) {
            ESP_LOGE(TAG, "HID close failed; preserving current connection state");
            break;
        }
        if (param->close.conn_status != ESP_HIDD_CONN_STATE_DISCONNECTED) {
            ESP_LOGI(TAG, "HID disconnect not complete; discovery remains unchanged");
            break;
        }
        wiimote_state_close();
        wiimote_web_set_hid_connected(false);
        close_sync_pairing_window("HID connection closed");
#if WIIMOTE_UPSTREAM_DISCOVERY_BASELINE
        maybe_arm_upstream_discovery("HID connection closed");
#endif
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
            close_sync_pairing_window("virtual cable unplugged");
        }
        break;
    default:
        ESP_LOGI(TAG, "HID event=%d", event);
        break;
    }
}

void app_main(void)
{
#if CONFIG_EXAMPLE_BT_DISCOVERY_TRACE
    ESP_ERROR_CHECK(wiimote_bt_trace_start());
#endif
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t controller_config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&controller_config));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));

    esp_bluedroid_config_t host_config = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    host_config.ssp_en = false;
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&host_config));
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    ESP_ERROR_CHECK(wiimote_discovery_init(discovery_error));
    ESP_ERROR_CHECK(esp_bt_gap_register_callback(on_gap_event));
    ESP_ERROR_CHECK(esp_bt_hid_device_register_callback(on_hid_event));
    ESP_ERROR_CHECK(esp_bt_gap_set_device_name(local_device_name));

    ESP_ERROR_CHECK(apply_wiimote_cod());

    s_app.name = "Nintendo RVL-CNT-01";
    s_app.description = "Nintendo RVL-CNT-01";
    s_app.provider = "Nintendo";
    s_app.subclass = ESP_HID_CLASS_JOS;
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

#if CONFIG_EXAMPLE_WIFI_WEB_CONTROL
    const esp_timer_create_args_t sync_timer_args = {
        .callback = sync_pairing_timeout,
        .name = "wiimote_sync",
    };
    ESP_ERROR_CHECK(esp_timer_create(&sync_timer_args, &s_sync_pairing_timer));
    wiimote_web_set_sync_callback(start_sync_pairing_window);
#endif

    ESP_ERROR_CHECK(esp_bt_hid_device_init());
#if CONFIG_BT_SDP_COMMON_ENABLED
    ESP_ERROR_CHECK(esp_sdp_register_callback(on_sdp_event));
    ESP_ERROR_CHECK(esp_sdp_init());
#endif
#if CONFIG_EXAMPLE_WIFI_WEB_CONTROL
    wiimote_controls_init();
    ir_camera_service_start();
#if WIIMOTE_DIAGNOSTIC_RADIO_ONLY
    ESP_LOGW(TAG, "RADIO-ONLY DIAGNOSTIC: Wi-Fi/web startup disabled; restore a radio-enabled build after the probe");
#else
    wiimote_web_start();
#endif
#endif
    esp_err_t imu_err = imu_service_start();
    if (imu_err != ESP_OK) {
        ESP_LOGW(TAG, "BNO08x orientation service unavailable: %s",
                 esp_err_to_name(imu_err));
    }
    ESP_ERROR_CHECK(wiimote_controls_start_fake_demo());
}

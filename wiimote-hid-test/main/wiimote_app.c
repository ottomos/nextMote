#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
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
#include "freertos/semphr.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "nvs.h"
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
#include "wiimote_accel.h"
#if CONFIG_EXAMPLE_BT_DISCOVERY_TRACE
#include "wiimote_bt_trace.h"
#endif

static const char *TAG = "espwiimote";
static const char local_device_name[] = CONFIG_EXAMPLE_LOCAL_DEVICE_NAME;
#define WIIMOTE_RECONNECT_INITIAL_DELAY_MS 1000U
#define WIIMOTE_RECONNECT_MAX_DELAY_MS 30000U
#define WIIMOTE_RECONNECT_INCOMING_GRACE_MS 10000U
static esp_hidd_app_param_t s_app;
static esp_hidd_qos_param_t s_qos;
static esp_timer_handle_t s_sync_pairing_timer;
static esp_timer_handle_t s_reconnect_timer;
#if CONFIG_EXAMPLE_WIFI_WEB_CONTROL
static esp_timer_handle_t s_connection_operation_timer;
#define WIIMOTE_CONNECTION_OPERATION_RETRY_MS 2000U
#define WIIMOTE_CONNECTION_OPERATION_MAX_RETRY_MS 8000U
#endif
static portMUX_TYPE s_sync_pairing_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_sync_pairing_active;
static portMUX_TYPE s_reconnect_mux = portMUX_INITIALIZER_UNLOCKED;
static esp_bd_addr_t s_last_host;
static bool s_last_host_valid;
static bool s_last_host_persisted;
static bool s_reconnect_paused;
#if CONFIG_EXAMPLE_WIFI_WEB_CONTROL
typedef enum {
    CONNECTION_OP_IDLE,
    CONNECTION_OP_PAIR_RESTARTING_BLUEDROID,
    CONNECTION_OP_PAIR_OPENING_WINDOW,
    CONNECTION_OP_PAIR_FAILED,
    CONNECTION_OP_RECONNECT_RESTARTING_BLUEDROID,
    CONNECTION_OP_RECONNECT_CONNECTING,
    CONNECTION_OP_RECONNECT_FAILED,
    CONNECTION_OP_SYNC_REARM_DISCONNECTING,
    CONNECTION_OP_SYNC_REARM_FAILED,
} connection_operation_state_t;
static connection_operation_state_t s_connection_operation;
static bool s_connection_operation_general;
static esp_bd_addr_t s_connection_operation_host;
static uint32_t s_connection_operation_retry_ms;
static SemaphoreHandle_t s_pair_bond_remove_done;
static SemaphoreHandle_t s_pair_vc_unplug_done;
static esp_bd_addr_t s_pair_bond_remove_target;
static esp_bt_status_t s_pair_bond_remove_status;
static esp_hidd_status_t s_pair_vc_unplug_status;
static esp_hidd_connection_state_t s_pair_vc_unplug_conn_status;
static bool s_pair_bond_remove_pending;
static bool s_pair_vc_unplug_pending;
static bool s_bluetooth_stack_restart_active;
#if CONFIG_BT_SDP_COMMON_ENABLED
static SemaphoreHandle_t s_pair_sdp_deinit_done;
static esp_sdp_status_t s_pair_sdp_deinit_status;
#endif
#endif
static uint32_t s_reconnect_delay_ms = WIIMOTE_RECONNECT_INITIAL_DELAY_MS;
static portMUX_TYPE s_device_id_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_device_id_record_ready;
static esp_bluetooth_sdp_record_t s_device_id_record;
#if WIIMOTE_UPSTREAM_DISCOVERY_BASELINE
static void maybe_arm_upstream_discovery(const char *reason);
#endif
#if CONFIG_EXAMPLE_WIFI_WEB_CONTROL
static connection_operation_state_t connection_operation_snapshot(
    esp_bd_addr_t host, bool *general_discoverable);
static esp_err_t resume_connection_operation(void);
static void connection_operation_timeout(void *arg);
static void on_gap_event(esp_bt_gap_cb_event_t event,
                         esp_bt_gap_cb_param_t *param);
static void on_hid_event(esp_hidd_cb_event_t event,
                         esp_hidd_cb_param_t *param);
static esp_err_t request_saved_host_connection(const esp_bd_addr_t host);
static esp_err_t request_bluetooth_stack_restart(
    connection_operation_state_t state, const esp_bd_addr_t host,
    bool general_discoverable);
static void bluetooth_stack_restart_task(void *arg);
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

#if !WIIMOTE_UPSTREAM_DISCOVERY_BASELINE
static void arm_bonded_host_reconnect(const char *reason)
{
    esp_err_t err = set_pairing_discoverability(false, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not hide discovery before reconnect mode (%s): %s",
                 reason, esp_err_to_name(err));
        return;
    }

    int bonded_count = esp_bt_gap_get_bond_device_num();
    if (bonded_count < 0) {
        ESP_LOGE(TAG, "Could not read bonded host count for reconnect mode (%s): %d",
                 reason, bonded_count);
        return;
    }
    if (bonded_count == 0) {
        ESP_LOGI(TAG, "RECONNECT_NOT_ARMED reason=%s bonded_hosts=0", reason);
        return;
    }

    err = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not enable bonded-host reconnect scan (%s): %s",
                 reason, esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG,
             "RECONNECT_READY reason=%s bonded_hosts=%d connectable=1 discoverable=0",
             reason, bonded_count);
}
#endif

static bool copy_last_host(esp_bd_addr_t host)
{
    portENTER_CRITICAL(&s_reconnect_mux);
    bool valid = s_last_host_valid;
    if (valid) memcpy(host, s_last_host, sizeof(s_last_host));
    portEXIT_CRITICAL(&s_reconnect_mux);
    return valid;
}

static esp_err_t is_bonded_peer(const esp_bd_addr_t peer, bool *bonded)
{
    if (!bonded) return ESP_ERR_INVALID_ARG;
    *bonded = false;
    if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
        return ESP_ERR_INVALID_STATE;
    }

    int count = esp_bt_gap_get_bond_device_num();
    if (count < 0) return ESP_FAIL;
    if (count == 0) return ESP_OK;
    if ((size_t)count > SIZE_MAX / sizeof(esp_bd_addr_t)) return ESP_ERR_INVALID_SIZE;

    esp_bd_addr_t *peers = calloc((size_t)count, sizeof(*peers));
    if (!peers) return ESP_ERR_NO_MEM;

    int actual = count;
    esp_err_t err = esp_bt_gap_get_bond_device_list(&actual, peers);
    if (err == ESP_OK &&
        (actual != count || esp_bt_gap_get_bond_device_num() != count)) {
        err = ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK) {
        for (int i = 0; i < actual; ++i) {
            if (memcmp(peer, peers[i], sizeof(esp_bd_addr_t)) == 0) {
                *bonded = true;
                break;
            }
        }
    }
    free(peers);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not verify bonded PIN requester: %s", esp_err_to_name(err));
    }
    return err;
}

static void save_last_host(const esp_bd_addr_t host)
{
    portENTER_CRITICAL(&s_reconnect_mux);
    bool same_host = s_last_host_valid &&
                     memcmp(s_last_host, host, sizeof(s_last_host)) == 0;
    if (!same_host) {
        memcpy(s_last_host, host, sizeof(s_last_host));
        s_last_host_valid = true;
        s_last_host_persisted = false;
    }
    s_reconnect_paused = false;
    s_reconnect_delay_ms = WIIMOTE_RECONNECT_INITIAL_DELAY_MS;
    bool persist = !s_last_host_persisted;
    portEXIT_CRITICAL(&s_reconnect_mux);
    wiimote_web_set_last_host(host);
    if (!persist) return;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wiimote", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not open NVS to save last HID host: %s",
                 esp_err_to_name(err));
        return;
    }
    err = nvs_set_blob(nvs, "last_host", host, sizeof(s_last_host));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not persist last HID host: %s", esp_err_to_name(err));
        return;
    }
    portENTER_CRITICAL(&s_reconnect_mux);
    s_last_host_persisted = true;
    portEXIT_CRITICAL(&s_reconnect_mux);
    ESP_LOGI(TAG, "Persisted last HID host for automatic reconnect");
}

static void load_last_host(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wiimote", NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not open NVS to load last HID host: %s",
                 esp_err_to_name(err));
        return;
    }

    esp_bd_addr_t host = {0};
    size_t length = sizeof(host);
    err = nvs_get_blob(nvs, "last_host", host, &length);
    nvs_close(nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not load last HID host: %s", esp_err_to_name(err));
        return;
    }
    if (length != sizeof(host)) {
        ESP_LOGE(TAG, "Ignoring malformed last HID host address length=%u",
                 (unsigned)length);
        return;
    }

    bool nonzero = false;
    for (size_t i = 0; i < sizeof(host); ++i) nonzero |= host[i] != 0;
    if (!nonzero) {
        ESP_LOGE(TAG, "Ignoring invalid zero last HID host address");
        return;
    }

    portENTER_CRITICAL(&s_reconnect_mux);
    memcpy(s_last_host, host, sizeof(s_last_host));
    s_last_host_valid = true;
    s_last_host_persisted = true;
    portEXIT_CRITICAL(&s_reconnect_mux);
    wiimote_web_set_last_host(host);
    ESP_LOGI(TAG, "Loaded saved HID host for automatic reconnect");
}

static void clear_last_host(void)
{
    portENTER_CRITICAL(&s_reconnect_mux);
    memset(s_last_host, 0, sizeof(s_last_host));
    s_last_host_valid = false;
    s_last_host_persisted = false;
    s_reconnect_paused = false;
    s_reconnect_delay_ms = WIIMOTE_RECONNECT_INITIAL_DELAY_MS;
    portEXIT_CRITICAL(&s_reconnect_mux);
    wiimote_web_set_last_host(NULL);
    wiimote_web_set_reconnect_pending(false);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wiimote", NVS_READWRITE, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not open NVS to clear last HID host: %s",
                 esp_err_to_name(err));
        return;
    }
    err = nvs_erase_key(nvs, "last_host");
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not clear persisted last HID host: %s",
                 esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "Cleared saved HID host after virtual cable unplug");
}

static void stop_hid_reconnect_timer(void)
{
    if (!s_reconnect_timer || !esp_timer_is_active(s_reconnect_timer)) {
        wiimote_web_set_reconnect_pending(false);
        return;
    }

    esp_err_t err = esp_timer_stop(s_reconnect_timer);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Could not stop HID reconnect timer: %s", esp_err_to_name(err));
        wiimote_web_set_reconnect_pending(esp_timer_is_active(s_reconnect_timer));
        return;
    }
    wiimote_web_set_reconnect_pending(false);
}

static void schedule_hid_reconnect(const char *reason)
{
    if (wiimote_state_is_connected()) {
        wiimote_web_set_reconnect_pending(false);
        return;
    }
    if (!s_reconnect_timer) {
        ESP_LOGE(TAG, "Cannot schedule HID reconnect before timer initialization");
        wiimote_web_set_reconnect_pending(false);
        return;
    }
    if (esp_timer_is_active(s_reconnect_timer)) {
        wiimote_web_set_reconnect_pending(true);
        return;
    }

    portENTER_CRITICAL(&s_reconnect_mux);
    bool reconnect_paused = s_reconnect_paused;
    uint32_t delay_ms = s_reconnect_delay_ms;
    portEXIT_CRITICAL(&s_reconnect_mux);
    if (reconnect_paused) {
        ESP_LOGW(TAG, "HID reconnect is paused (%s)", reason);
        wiimote_web_set_reconnect_pending(false);
        return;
    }

    esp_bd_addr_t host;
    if (!copy_last_host(host)) {
        ESP_LOGI(TAG, "RECONNECT_NOT_ARMED reason=%s last_host=unknown", reason);
        wiimote_web_set_reconnect_pending(false);
        return;
    }
    bool bonded = false;
    esp_err_t err = is_bonded_peer(host, &bonded);
    if (err != ESP_OK) {
        wiimote_web_set_reconnect_pending(false);
        return;
    }
    if (!bonded) {
        ESP_LOGW(TAG, "Saved HID host is no longer bonded; clearing reconnect target");
        clear_last_host();
        return;
    }

    portENTER_CRITICAL(&s_reconnect_mux);
    if (s_reconnect_delay_ms < WIIMOTE_RECONNECT_MAX_DELAY_MS) {
        uint32_t next_delay = s_reconnect_delay_ms * 2U;
        s_reconnect_delay_ms = next_delay > WIIMOTE_RECONNECT_MAX_DELAY_MS
                                   ? WIIMOTE_RECONNECT_MAX_DELAY_MS : next_delay;
    }
    portEXIT_CRITICAL(&s_reconnect_mux);

    err = esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_ms * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not schedule HID reconnect after %s: %s",
                 reason, esp_err_to_name(err));
        wiimote_web_set_reconnect_pending(false);
        return;
    }
    wiimote_web_set_reconnect_pending(true);
    ESP_LOGI(TAG, "HID_RECONNECT_SCHEDULED reason=%s delay_ms=%u",
             reason, (unsigned)delay_ms);
}

static void hid_reconnect_timeout(void *arg)
{
    (void)arg;
    if (wiimote_state_is_connected()) return;

    esp_bd_addr_t host;
    if (!copy_last_host(host)) return;
    bool bonded = false;
    esp_err_t err = is_bonded_peer(host, &bonded);
    if (err != ESP_OK) {
        schedule_hid_reconnect("could not verify saved bond");
        return;
    }
    if (!bonded) {
        ESP_LOGW(TAG, "Saved HID host is no longer bonded; clearing reconnect target");
        clear_last_host();
        return;
    }

    err = esp_bt_hid_device_connect(host);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HID reconnect request failed: %s", esp_err_to_name(err));
        schedule_hid_reconnect("connect request failed");
        return;
    }
    ESP_LOGI(TAG, "HID_RECONNECT_REQUESTED");
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
#if CONFIG_EXAMPLE_WIFI_WEB_CONTROL
            connection_operation_state_t operation =
                connection_operation_snapshot(NULL, NULL);
            if (operation == CONNECTION_OP_PAIR_RESTARTING_BLUEDROID ||
                operation == CONNECTION_OP_RECONNECT_RESTARTING_BLUEDROID) {
                esp_err_t resume_err = resume_connection_operation();
                if (resume_err != ESP_OK &&
                    resume_err != ESP_ERR_INVALID_STATE) {
                    ESP_LOGE(TAG, "Could not continue Bluetooth restart after SDP ready: %s",
                             esp_err_to_name(resume_err));
                }
            }
#endif
        } else {
            ESP_LOGE(TAG, "Nintendo Device ID SDP record creation failed: status=%d",
                     param->create_record.status);
        }
        break;
    }
    case ESP_SDP_DEINIT_EVT:
        set_device_id_record_ready(false);
#if CONFIG_EXAMPLE_WIFI_WEB_CONTROL
        s_pair_sdp_deinit_status = param->deinit.status;
        if (s_pair_sdp_deinit_done &&
            xSemaphoreGive(s_pair_sdp_deinit_done) != pdTRUE) {
            ESP_LOGE(TAG, "Could not signal SDP deinitialization completion");
        }
#endif
        ESP_LOGI(TAG, "SDP deinitialized result=%d", param->deinit.status);
        break;
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

#if WIIMOTE_UPSTREAM_DISCOVERY_BASELINE
    esp_err_t err = set_pairing_discoverability(false, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not close expired SYNC window: %s", esp_err_to_name(err));
        return;
    }
#else
    arm_bonded_host_reconnect("SYNC window expired");
#endif
    schedule_hid_reconnect("SYNC window expired");
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

#if CONFIG_EXAMPLE_WIFI_WEB_CONTROL
static esp_err_t start_sync_pairing_window(bool general_discoverable);

static bool connection_operation_is_pair(connection_operation_state_t state)
{
    switch (state) {
    case CONNECTION_OP_PAIR_RESTARTING_BLUEDROID:
    case CONNECTION_OP_PAIR_OPENING_WINDOW:
    case CONNECTION_OP_PAIR_FAILED:
        return true;
    default:
        return false;
    }
}

static bool connection_operation_is_reconnect(connection_operation_state_t state)
{
    return state == CONNECTION_OP_RECONNECT_RESTARTING_BLUEDROID ||
           state == CONNECTION_OP_RECONNECT_CONNECTING ||
           state == CONNECTION_OP_RECONNECT_FAILED;
}

static const char *connection_operation_phase_name(connection_operation_state_t state)
{
    switch (state) {
    case CONNECTION_OP_PAIR_RESTARTING_BLUEDROID:
        return "restarting-bluetooth";
    case CONNECTION_OP_PAIR_OPENING_WINDOW: return "opening-window";
    case CONNECTION_OP_PAIR_FAILED:
    case CONNECTION_OP_RECONNECT_FAILED:
    case CONNECTION_OP_SYNC_REARM_FAILED:
        return "failed";
    case CONNECTION_OP_RECONNECT_RESTARTING_BLUEDROID:
        return "reconnect-restarting-bluetooth";
    case CONNECTION_OP_RECONNECT_CONNECTING:
        return "reconnect-connecting";
    case CONNECTION_OP_SYNC_REARM_DISCONNECTING:
        return "rearm-disconnecting";
    case CONNECTION_OP_IDLE:
    default:
        return "idle";
    }
}

static connection_operation_state_t connection_operation_snapshot(
    esp_bd_addr_t host, bool *general_discoverable)
{
    portENTER_CRITICAL(&s_reconnect_mux);
    connection_operation_state_t state = s_connection_operation;
    if (host != NULL) {
        memcpy(host, s_connection_operation_host,
               sizeof(s_connection_operation_host));
    }
    if (general_discoverable != NULL) {
        *general_discoverable = s_connection_operation_general;
    }
    portEXIT_CRITICAL(&s_reconnect_mux);
    return state;
}

static bool bluetooth_stack_restart_active_snapshot(void)
{
    portENTER_CRITICAL(&s_reconnect_mux);
    bool active = s_bluetooth_stack_restart_active;
    portEXIT_CRITICAL(&s_reconnect_mux);
    return active;
}

static void set_bluetooth_stack_restart_active(bool active)
{
    portENTER_CRITICAL(&s_reconnect_mux);
    s_bluetooth_stack_restart_active = active;
    portEXIT_CRITICAL(&s_reconnect_mux);
}

static void set_stack_bond_remove_pending(const esp_bd_addr_t host,
                                          bool pending)
{
    portENTER_CRITICAL(&s_reconnect_mux);
    s_pair_bond_remove_pending = pending;
    if (pending && host != NULL) {
        memcpy(s_pair_bond_remove_target, host,
               sizeof(s_pair_bond_remove_target));
    } else if (!pending) {
        memset(s_pair_bond_remove_target, 0,
               sizeof(s_pair_bond_remove_target));
    }
    portEXIT_CRITICAL(&s_reconnect_mux);
}

static bool stack_bond_remove_pending_for_peer(const esp_bd_addr_t host)
{
    portENTER_CRITICAL(&s_reconnect_mux);
    static const esp_bd_addr_t zero_address = {0};
    bool any_peer = memcmp(s_pair_bond_remove_target, zero_address,
                           sizeof(s_pair_bond_remove_target)) == 0;
    bool pending = s_pair_bond_remove_pending &&
                   (any_peer ||
                    memcmp(s_pair_bond_remove_target, host,
                           sizeof(s_pair_bond_remove_target)) == 0);
    portEXIT_CRITICAL(&s_reconnect_mux);
    return pending;
}

static void connection_operation_cancel_watchdog(void)
{
    if (!s_connection_operation_timer ||
        !esp_timer_is_active(s_connection_operation_timer)) {
        return;
    }
    esp_err_t err = esp_timer_stop(s_connection_operation_timer);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Could not stop connection-operation watchdog: %s",
                 esp_err_to_name(err));
    }
}

static esp_err_t connection_operation_arm_watchdog(void)
{
    if (!s_connection_operation_timer) {
        ESP_LOGE(TAG, "Connection-operation watchdog is not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    connection_operation_cancel_watchdog();
    portENTER_CRITICAL(&s_reconnect_mux);
    uint32_t delay_ms = s_connection_operation_retry_ms;
    portEXIT_CRITICAL(&s_reconnect_mux);
    if (delay_ms == 0) delay_ms = WIIMOTE_CONNECTION_OPERATION_RETRY_MS;
    esp_err_t err = esp_timer_start_once(s_connection_operation_timer,
                                         (uint64_t)delay_ms * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not arm connection-operation watchdog: %s",
                 esp_err_to_name(err));
    } else {
        uint32_t next_delay = delay_ms * 2U;
        portENTER_CRITICAL(&s_reconnect_mux);
        s_connection_operation_retry_ms =
            next_delay > WIIMOTE_CONNECTION_OPERATION_MAX_RETRY_MS
                ? WIIMOTE_CONNECTION_OPERATION_MAX_RETRY_MS : next_delay;
        portEXIT_CRITICAL(&s_reconnect_mux);
    }
    return err;
}

static void connection_operation_set_state(connection_operation_state_t state)
{
    connection_operation_cancel_watchdog();
    portENTER_CRITICAL(&s_reconnect_mux);
    s_connection_operation = state;
    if (state == CONNECTION_OP_IDLE) {
        s_connection_operation_general = false;
        memset(s_connection_operation_host, 0,
               sizeof(s_connection_operation_host));
        s_connection_operation_retry_ms = WIIMOTE_CONNECTION_OPERATION_RETRY_MS;
    }
    portEXIT_CRITICAL(&s_reconnect_mux);

    wiimote_web_set_pairing_phase(
        connection_operation_is_pair(state)
            ? connection_operation_phase_name(state) : "idle");
    wiimote_web_set_manual_reconnect_pending(
        connection_operation_is_reconnect(state));
}

static void connection_operation_begin(connection_operation_state_t state,
                                       const esp_bd_addr_t host,
                                       bool general_discoverable)
{
    connection_operation_cancel_watchdog();
    portENTER_CRITICAL(&s_reconnect_mux);
    connection_operation_state_t previous_state = s_connection_operation;
    s_connection_operation = state;
    s_connection_operation_general = general_discoverable;
    memcpy(s_connection_operation_host, host,
           sizeof(s_connection_operation_host));
    bool same_flow =
        (connection_operation_is_pair(previous_state) &&
         connection_operation_is_pair(state)) ||
        (connection_operation_is_reconnect(previous_state) &&
         connection_operation_is_reconnect(state)) ||
        ((previous_state == CONNECTION_OP_SYNC_REARM_DISCONNECTING ||
          previous_state == CONNECTION_OP_SYNC_REARM_FAILED) &&
         (state == CONNECTION_OP_SYNC_REARM_DISCONNECTING ||
          state == CONNECTION_OP_SYNC_REARM_FAILED));
    if (!same_flow) {
        s_connection_operation_retry_ms =
            WIIMOTE_CONNECTION_OPERATION_RETRY_MS;
    }
    portEXIT_CRITICAL(&s_reconnect_mux);

    wiimote_web_set_pairing_phase(
        connection_operation_is_pair(state)
            ? connection_operation_phase_name(state) : "idle");
    wiimote_web_set_manual_reconnect_pending(
        connection_operation_is_reconnect(state));
}

static esp_err_t start_pair_window_after_release(const esp_bd_addr_t host,
                                                  bool general_discoverable)
{
    char peer[18];
    esp_bd_addr_t printable_host;
    memcpy(printable_host, host, sizeof(printable_host));
    ESP_LOGI(TAG, "HOST_SWITCH_OLD_HOST_RELEASED peer=%s",
             wiimote_bt_address_string(printable_host, peer));
    clear_last_host();
    connection_operation_set_state(CONNECTION_OP_PAIR_OPENING_WINDOW);
    esp_err_t err = start_sync_pairing_window(general_discoverable);
    if (err != ESP_OK) {
        connection_operation_set_state(CONNECTION_OP_PAIR_FAILED);
        ESP_LOGE(TAG, "Could not open SYNC after saved host release: %s",
                 esp_err_to_name(err));
        (void)connection_operation_arm_watchdog();
        return err;
    }
    connection_operation_set_state(CONNECTION_OP_IDLE);
    return ESP_OK;
}

static bool bluetooth_address_is_zero(const esp_bd_addr_t address)
{
    static const esp_bd_addr_t zero_address = {0};
    return memcmp(address, zero_address, sizeof(zero_address)) == 0;
}

static esp_err_t remove_bond_and_wait(const esp_bd_addr_t host)
{
    bool bonded = false;
    esp_err_t err = is_bonded_peer(host, &bonded);
    if (err != ESP_OK || !bonded) return err;
    if (!s_pair_bond_remove_done) return ESP_ERR_INVALID_STATE;

    while (xSemaphoreTake(s_pair_bond_remove_done, 0) == pdTRUE) {
    }
    set_stack_bond_remove_pending(host, true);

    esp_bd_addr_t removal_host;
    memcpy(removal_host, host, sizeof(removal_host));
    err = esp_bt_gap_remove_bond_device(removal_host);
    if (err != ESP_OK) {
        set_stack_bond_remove_pending(host, false);
        ESP_LOGE(TAG, "Could not request Bluetooth bond removal: %s",
                 esp_err_to_name(err));
        return err;
    }

    if (xSemaphoreTake(s_pair_bond_remove_done,
                       pdMS_TO_TICKS(5000)) != pdTRUE) {
        set_stack_bond_remove_pending(host, false);
        ESP_LOGW(TAG, "Timed out waiting for Bluetooth bond-removal callback");
    } else {
        set_stack_bond_remove_pending(host, false);
        if (s_pair_bond_remove_status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGW(TAG, "Bluetooth bond-removal callback status=%u",
                     (unsigned)s_pair_bond_remove_status);
        }
    }

    bonded = false;
    err = is_bonded_peer(host, &bonded);
    if (err != ESP_OK) return err;
    if (bonded) {
        ESP_LOGE(TAG, "Bluetooth bond remains after targeted removal");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t remove_pair_bonds(const esp_bd_addr_t saved_host)
{
    if (!bluetooth_address_is_zero(saved_host)) {
        esp_err_t err = remove_bond_and_wait(saved_host);
        if (err != ESP_OK) return err;
    }

    int bond_count = esp_bt_gap_get_bond_device_num();
    if (bond_count < 0) {
        ESP_LOGE(TAG, "Could not read Bluetooth bond count");
        return ESP_FAIL;
    }
    if (bond_count == 0) return ESP_OK;

    esp_bd_addr_t *bonded_hosts =
        calloc((size_t)bond_count, sizeof(*bonded_hosts));
    if (!bonded_hosts) {
        ESP_LOGE(TAG, "Could not allocate Bluetooth bond list");
        return ESP_ERR_NO_MEM;
    }

    int listed = bond_count;
    esp_err_t err = esp_bt_gap_get_bond_device_list(&listed, bonded_hosts);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not list Bluetooth bonds: %s", esp_err_to_name(err));
        free(bonded_hosts);
        return err;
    }

    for (int i = 0; i < listed; ++i) {
        if (!bluetooth_address_is_zero(saved_host) &&
            memcmp(bonded_hosts[i], saved_host, ESP_BD_ADDR_LEN) == 0) {
            continue;
        }
        err = remove_bond_and_wait(bonded_hosts[i]);
        if (err != ESP_OK) break;
    }
    free(bonded_hosts);
    return err;
}

static esp_err_t release_pair_virtual_cable(void)
{
    esp_hidd_profile_status_t profile = {0};
    esp_err_t err = esp_bt_hid_device_get_profile_status(&profile);
    if (err != ESP_OK) return err;
    if (profile.plug_vc_dev_num == 0) return ESP_OK;
    if (!s_pair_vc_unplug_done || !s_pair_bond_remove_done) {
        return ESP_ERR_INVALID_STATE;
    }

    while (xSemaphoreTake(s_pair_vc_unplug_done, 0) == pdTRUE) {
    }
    while (xSemaphoreTake(s_pair_bond_remove_done, 0) == pdTRUE) {
    }

    esp_bd_addr_t host = {0};
    (void)connection_operation_snapshot(host, NULL);
    set_stack_bond_remove_pending(host, true);
    portENTER_CRITICAL(&s_reconnect_mux);
    s_pair_vc_unplug_pending = true;
    portEXIT_CRITICAL(&s_reconnect_mux);

    err = esp_bt_hid_device_virtual_cable_unplug();
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_reconnect_mux);
        s_pair_vc_unplug_pending = false;
        portEXIT_CRITICAL(&s_reconnect_mux);
        set_stack_bond_remove_pending(host, false);
        return err;
    }

    if (xSemaphoreTake(s_pair_vc_unplug_done, pdMS_TO_TICKS(5000)) != pdTRUE) {
        portENTER_CRITICAL(&s_reconnect_mux);
        s_pair_vc_unplug_pending = false;
        portEXIT_CRITICAL(&s_reconnect_mux);
        set_stack_bond_remove_pending(host, false);
        return ESP_ERR_TIMEOUT;
    }

    if (s_pair_vc_unplug_status != ESP_HIDD_SUCCESS ||
        s_pair_vc_unplug_conn_status != ESP_HIDD_CONN_STATE_DISCONNECTED) {
        set_stack_bond_remove_pending(host, false);
        return ESP_FAIL;
    }

    bool bonded = false;
    if (!bluetooth_address_is_zero(host)) {
        err = is_bonded_peer(host, &bonded);
        if (err != ESP_OK) {
            set_stack_bond_remove_pending(host, false);
            return err;
        }
    } else {
        int bond_count = esp_bt_gap_get_bond_device_num();
        if (bond_count < 0) {
            set_stack_bond_remove_pending(host, false);
            return ESP_FAIL;
        }
        bonded = bond_count > 0;
    }

    if (bonded &&
        xSemaphoreTake(s_pair_bond_remove_done, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "Timed out waiting for VCU-triggered bond removal");
    }
    set_stack_bond_remove_pending(host, false);
    ESP_LOGI(TAG, "Pair virtual cable released before Bluetooth stack restart");
    return ESP_OK;
}

static esp_err_t request_bluetooth_stack_restart(
    connection_operation_state_t state, const esp_bd_addr_t host,
    bool general_discoverable)
{
    if (state != CONNECTION_OP_PAIR_RESTARTING_BLUEDROID &&
        state != CONNECTION_OP_RECONNECT_RESTARTING_BLUEDROID) {
        return ESP_ERR_INVALID_ARG;
    }

    stop_hid_reconnect_timer();
    close_sync_pairing_window("Bluetooth host restart requested");
    portENTER_CRITICAL(&s_reconnect_mux);
    s_reconnect_paused = true;
    bool restart_active = s_bluetooth_stack_restart_active;
    portEXIT_CRITICAL(&s_reconnect_mux);
    connection_operation_begin(state, host, general_discoverable);
    if (restart_active) return ESP_OK;

    set_bluetooth_stack_restart_active(true);
    BaseType_t task_result = xTaskCreate(
        bluetooth_stack_restart_task, "bt_host_restart", 6144, NULL, 5, NULL);
    if (task_result != pdPASS) {
        set_bluetooth_stack_restart_active(false);
        connection_operation_set_state(
            state == CONNECTION_OP_PAIR_RESTARTING_BLUEDROID
                ? CONNECTION_OP_PAIR_FAILED : CONNECTION_OP_RECONNECT_FAILED);
        ESP_LOGE(TAG, "Could not create Bluetooth stack restart task");
        (void)connection_operation_arm_watchdog();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t resume_connection_operation(void)
{
    esp_bd_addr_t host = {0};
    bool general_discoverable = false;
    connection_operation_state_t state =
        connection_operation_snapshot(host, &general_discoverable);

    switch (state) {
    case CONNECTION_OP_PAIR_RESTARTING_BLUEDROID:
        if (bluetooth_stack_restart_active_snapshot()) return ESP_OK;
        if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
            return request_bluetooth_stack_restart(
                CONNECTION_OP_PAIR_RESTARTING_BLUEDROID, host,
                general_discoverable);
        }
        if (!wiimote_sdp_hid_record_is_ready()) return ESP_ERR_INVALID_STATE;
#if CONFIG_BT_SDP_COMMON_ENABLED
        if (!device_id_record_is_ready()) return ESP_ERR_INVALID_STATE;
#endif
        return start_pair_window_after_release(host, general_discoverable);
    case CONNECTION_OP_PAIR_OPENING_WINDOW: {
        esp_err_t err = start_sync_pairing_window(general_discoverable);
        if (err == ESP_OK) connection_operation_set_state(CONNECTION_OP_IDLE);
        return err;
    }
    case CONNECTION_OP_PAIR_FAILED:
        return request_bluetooth_stack_restart(
            CONNECTION_OP_PAIR_RESTARTING_BLUEDROID, host,
            general_discoverable);
    case CONNECTION_OP_RECONNECT_RESTARTING_BLUEDROID:
        if (bluetooth_stack_restart_active_snapshot()) return ESP_OK;
        if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
            return request_bluetooth_stack_restart(
                CONNECTION_OP_RECONNECT_RESTARTING_BLUEDROID, host, false);
        }
        if (!wiimote_sdp_hid_record_is_ready()) return ESP_ERR_INVALID_STATE;
#if CONFIG_BT_SDP_COMMON_ENABLED
        if (!device_id_record_is_ready()) return ESP_ERR_INVALID_STATE;
#endif
        return request_saved_host_connection(host);
    case CONNECTION_OP_RECONNECT_CONNECTING:
    case CONNECTION_OP_RECONNECT_FAILED: {
        esp_hidd_profile_status_t profile = {0};
        esp_err_t err = esp_bt_hid_device_get_profile_status(&profile);
        if (err != ESP_OK) return err;
        if (profile.conn_num != 0 || wiimote_state_is_connected()) {
            connection_operation_set_state(CONNECTION_OP_IDLE);
            stop_hid_reconnect_timer();
            return ESP_OK;
        }
        if (state == CONNECTION_OP_RECONNECT_FAILED) {
            return request_bluetooth_stack_restart(
                CONNECTION_OP_RECONNECT_RESTARTING_BLUEDROID, host, false);
        }
        return request_saved_host_connection(host);
    }
    case CONNECTION_OP_SYNC_REARM_DISCONNECTING: {
        esp_hidd_profile_status_t profile = {0};
        esp_err_t err = esp_bt_hid_device_get_profile_status(&profile);
        if (err != ESP_OK) return err;
        if (profile.conn_num == 0) {
            wiimote_state_close();
            wiimote_web_set_hid_connected(false);
            err = start_sync_pairing_window(false);
            if (err == ESP_OK) connection_operation_set_state(CONNECTION_OP_IDLE);
            else {
                connection_operation_set_state(CONNECTION_OP_SYNC_REARM_FAILED);
                (void)connection_operation_arm_watchdog();
            }
            return err;
        }
        err = esp_bt_hid_device_disconnect();
        if (err == ESP_OK) (void)connection_operation_arm_watchdog();
        else {
            connection_operation_set_state(CONNECTION_OP_SYNC_REARM_FAILED);
            (void)connection_operation_arm_watchdog();
        }
        return err;
    }
    case CONNECTION_OP_SYNC_REARM_FAILED:
        if (wiimote_state_is_connected()) {
            connection_operation_begin(CONNECTION_OP_SYNC_REARM_DISCONNECTING,
                                       host, false);
            esp_err_t err = esp_bt_hid_device_disconnect();
            (void)connection_operation_arm_watchdog();
            return err;
        }
        {
            esp_err_t err = start_sync_pairing_window(false);
            if (err == ESP_OK) connection_operation_set_state(CONNECTION_OP_IDLE);
            else (void)connection_operation_arm_watchdog();
            return err;
        }
    case CONNECTION_OP_IDLE:
    default:
        return ESP_ERR_NOT_FOUND;
    }
}

static void connection_operation_timeout(void *arg)
{
    (void)arg;
    connection_operation_state_t state =
        connection_operation_snapshot(NULL, NULL);
    if (state == CONNECTION_OP_IDLE) return;
    if ((state == CONNECTION_OP_PAIR_RESTARTING_BLUEDROID ||
         state == CONNECTION_OP_RECONNECT_RESTARTING_BLUEDROID) &&
        bluetooth_stack_restart_active_snapshot()) {
        return;
    }

    ESP_LOGW(TAG, "Connection operation watchdog phase=%s; reconciling",
             connection_operation_phase_name(state));
    esp_err_t err = resume_connection_operation();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Connection operation retry failed: %s",
                 esp_err_to_name(err));
    }

    state = connection_operation_snapshot(NULL, NULL);
    if (state != CONNECTION_OP_IDLE &&
        !esp_timer_is_active(s_connection_operation_timer)) {
        (void)connection_operation_arm_watchdog();
    }
}

static void bluetooth_stack_restart_failed(const char *phase, esp_err_t err)
{
    connection_operation_state_t state =
        connection_operation_snapshot(NULL, NULL);
    set_bluetooth_stack_restart_active(false);
    ESP_LOGE(TAG, "Bluetooth stack restart failed at %s: %s",
             phase, esp_err_to_name(err));
    if (state == CONNECTION_OP_PAIR_RESTARTING_BLUEDROID) {
        connection_operation_set_state(CONNECTION_OP_PAIR_FAILED);
    } else if (state == CONNECTION_OP_RECONNECT_RESTARTING_BLUEDROID) {
        connection_operation_set_state(CONNECTION_OP_RECONNECT_FAILED);
    }
    (void)connection_operation_arm_watchdog();
}

static void bluetooth_stack_restart_task(void *arg)
{
    (void)arg;
    esp_bd_addr_t host = {0};
    bool general_discoverable = false;
    connection_operation_state_t state =
        connection_operation_snapshot(host, &general_discoverable);
    bool pair_restart = state == CONNECTION_OP_PAIR_RESTARTING_BLUEDROID;
    bool reconnect_restart =
        state == CONNECTION_OP_RECONNECT_RESTARTING_BLUEDROID;
    if (!pair_restart && !reconnect_restart) {
        bluetooth_stack_restart_failed("operation validation",
                                       ESP_ERR_INVALID_STATE);
        vTaskDelete(NULL);
        return;
    }

    bool pair_bonds_removed = false;
    if (pair_restart) {
        esp_err_t unplug_err = release_pair_virtual_cable();
        if (unplug_err != ESP_OK) {
            ESP_LOGW(TAG, "Pair virtual-cable release did not complete before restart: %s",
                     esp_err_to_name(unplug_err));
        }
        esp_err_t bond_err = remove_pair_bonds(host);
        if (bond_err == ESP_OK) {
            pair_bonds_removed = true;
        } else {
            ESP_LOGW(TAG, "Pair bond cleanup before Bluetooth restart was incomplete: %s",
                     esp_err_to_name(bond_err));
        }
    }

    wiimote_state_close();
    wiimote_web_set_hid_connected(false);
    stop_hid_reconnect_timer();
    close_sync_pairing_window("Bluetooth host restart");
    esp_err_t err = wiimote_discovery_set_rotation(false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Inquiry rotation stop before Bluetooth restart failed: %s",
                 esp_err_to_name(err));
    }

#if CONFIG_BT_SDP_COMMON_ENABLED
    esp_sdp_protocol_status_t sdp_status = {0};
    err = esp_sdp_get_protocol_status(&sdp_status);
    if (err != ESP_OK) {
        bluetooth_stack_restart_failed("SDP status", err);
        vTaskDelete(NULL);
        return;
    }
    if (sdp_status.sdp_inited) {
        while (xSemaphoreTake(s_pair_sdp_deinit_done, 0) == pdTRUE) {
        }
        err = esp_sdp_deinit();
        if (err != ESP_OK) {
            bluetooth_stack_restart_failed("SDP deinit request", err);
            vTaskDelete(NULL);
            return;
        }
        if (xSemaphoreTake(s_pair_sdp_deinit_done,
                           pdMS_TO_TICKS(7000)) != pdTRUE) {
            bluetooth_stack_restart_failed("SDP deinit callback",
                                           ESP_ERR_TIMEOUT);
            vTaskDelete(NULL);
            return;
        }
        if (s_pair_sdp_deinit_status != ESP_SDP_SUCCESS) {
            bluetooth_stack_restart_failed("SDP deinit completion",
                                           ESP_FAIL);
            vTaskDelete(NULL);
            return;
        }
    }
#endif

    err = esp_bluedroid_disable();
    if (err != ESP_OK) {
        bluetooth_stack_restart_failed("Bluedroid disable", err);
        vTaskDelete(NULL);
        return;
    }
    wiimote_sdp_clear_hid_ready();
    set_device_id_record_ready(false);

    err = esp_bluedroid_enable();
    if (err != ESP_OK) {
        bluetooth_stack_restart_failed("Bluedroid enable", err);
        vTaskDelete(NULL);
        return;
    }
    err = esp_bt_gap_register_callback(on_gap_event);
    if (err != ESP_OK) {
        bluetooth_stack_restart_failed("GAP callback registration", err);
        vTaskDelete(NULL);
        return;
    }
    err = esp_bt_hid_device_register_callback(on_hid_event);
    if (err != ESP_OK) {
        bluetooth_stack_restart_failed("HID callback registration", err);
        vTaskDelete(NULL);
        return;
    }
#if CONFIG_BT_SDP_COMMON_ENABLED
    err = esp_sdp_register_callback(on_sdp_event);
    if (err != ESP_OK) {
        bluetooth_stack_restart_failed("SDP callback registration", err);
        vTaskDelete(NULL);
        return;
    }
#endif
    err = esp_bt_gap_set_device_name(local_device_name);
    if (err != ESP_OK) {
        bluetooth_stack_restart_failed("device name", err);
        vTaskDelete(NULL);
        return;
    }
    err = apply_wiimote_cod();
    if (err != ESP_OK) {
        bluetooth_stack_restart_failed("Class of Device", err);
        vTaskDelete(NULL);
        return;
    }
    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
    esp_bt_pin_code_t empty_pin = {0};
    err = esp_bt_gap_set_pin(pin_type, 0, empty_pin);
    if (err != ESP_OK) {
        bluetooth_stack_restart_failed("legacy PIN setup", err);
        vTaskDelete(NULL);
        return;
    }

    if (pair_restart) {
        if (!pair_bonds_removed) {
            err = remove_pair_bonds(host);
            if (err != ESP_OK) {
                bluetooth_stack_restart_failed("Pair bond removal", err);
                vTaskDelete(NULL);
                return;
            }
        }
        clear_last_host();
    }

    err = esp_bt_hid_device_init();
    if (err != ESP_OK) {
        bluetooth_stack_restart_failed("HID profile init", err);
        vTaskDelete(NULL);
        return;
    }
#if CONFIG_BT_SDP_COMMON_ENABLED
    err = esp_sdp_init();
    if (err != ESP_OK) {
        bluetooth_stack_restart_failed("SDP init", err);
        vTaskDelete(NULL);
        return;
    }
#endif

    set_bluetooth_stack_restart_active(false);
    ESP_LOGI(TAG, "Bluetooth host restarted; waiting for HID and SDP registration");
    err = resume_connection_operation();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Could not continue after Bluetooth host restart: %s",
                 esp_err_to_name(err));
    }
    connection_operation_state_t final_state =
        connection_operation_snapshot(NULL, NULL);
    if (final_state != CONNECTION_OP_IDLE &&
        !esp_timer_is_active(s_connection_operation_timer)) {
        (void)connection_operation_arm_watchdog();
    }
    vTaskDelete(NULL);
}

static esp_err_t queue_virtual_cable_unplug(void)
{
    esp_bd_addr_t host;
    if (!copy_last_host(host)) {
        ESP_LOGW(TAG, "No saved HID host is available for virtual-cable unplug");
        return ESP_ERR_NOT_FOUND;
    }
    bool bonded = false;
    esp_err_t err = is_bonded_peer(host, &bonded);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not verify saved HID host before virtual-cable unplug: %s",
                 esp_err_to_name(err));
        return err;
    }
    if (!bonded) {
        ESP_LOGW(TAG, "Saved HID host is not bonded; refusing virtual-cable unplug");
        return ESP_ERR_NOT_FOUND;
    }

    stop_hid_reconnect_timer();
    portENTER_CRITICAL(&s_reconnect_mux);
    s_reconnect_paused = true;
    portEXIT_CRITICAL(&s_reconnect_mux);

    close_sync_pairing_window("virtual-cable unplug requested");
    err = esp_bt_hid_device_virtual_cable_unplug();
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_reconnect_mux);
        s_reconnect_paused = false;
        portEXIT_CRITICAL(&s_reconnect_mux);
        ESP_LOGE(TAG, "Could not queue virtual-cable unplug: %s", esp_err_to_name(err));
        schedule_hid_reconnect("virtual-cable unplug request failed");
        return err;
    }

    ESP_LOGW(TAG, "VIRTUAL_CABLE_UNPLUG_REQUESTED for saved HID host");
    return ESP_OK;
}

static esp_err_t request_sync_with_host_switch(bool general_discoverable)
{
    connection_operation_state_t state =
        connection_operation_snapshot(NULL, NULL);
    if (connection_operation_is_pair(state)) {
        ESP_LOGW(TAG, "Resuming Pair host transition at phase=%s",
                 connection_operation_phase_name(state));
        return resume_connection_operation();
    }
    if (state != CONNECTION_OP_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_bd_addr_t host = {0};
    (void)copy_last_host(host);
    return request_bluetooth_stack_restart(
        CONNECTION_OP_PAIR_RESTARTING_BLUEDROID, host,
        general_discoverable);
}
#endif

#if CONFIG_EXAMPLE_BT_DISCOVERY_TRACE
static esp_err_t request_virtual_cable_unplug(void)
{
    return queue_virtual_cable_unplug();
}
#endif

static void discovery_error(esp_err_t error)
{
    ESP_LOGE(TAG, "Closing SYNC after discovery failure: %s", esp_err_to_name(error));
    close_sync_pairing_window("inquiry rotation failed");
#if !WIIMOTE_UPSTREAM_DISCOVERY_BASELINE
    arm_bonded_host_reconnect("inquiry rotation failed");
#endif
    schedule_hid_reconnect("inquiry rotation failed");
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

    stop_hid_reconnect_timer();
    portENTER_CRITICAL(&s_reconnect_mux);
    s_reconnect_paused = false;
    s_reconnect_delay_ms = WIIMOTE_RECONNECT_INITIAL_DELAY_MS;
    portEXIT_CRITICAL(&s_reconnect_mux);
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

#if CONFIG_EXAMPLE_WIFI_WEB_CONTROL
static esp_err_t request_sync_after_disconnect(void)
{
    if (connection_operation_snapshot(NULL, NULL) != CONNECTION_OP_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!wiimote_state_is_connected()) {
        return start_sync_pairing_window(false);
    }

    stop_hid_reconnect_timer();
    esp_bd_addr_t host = {0};
    (void)copy_last_host(host);
    portENTER_CRITICAL(&s_reconnect_mux);
    if (s_connection_operation != CONNECTION_OP_IDLE) {
        portEXIT_CRITICAL(&s_reconnect_mux);
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(s_connection_operation_host, host, sizeof(s_connection_operation_host));
    s_connection_operation_general = false;
    s_connection_operation = CONNECTION_OP_SYNC_REARM_DISCONNECTING;
    s_connection_operation_retry_ms = WIIMOTE_CONNECTION_OPERATION_RETRY_MS;
    s_reconnect_paused = true;
    portEXIT_CRITICAL(&s_reconnect_mux);
    wiimote_web_set_pairing_phase("rearm-disconnecting");
    wiimote_web_set_manual_reconnect_pending(false);

    close_sync_pairing_window("re-pair requested");
    esp_err_t err = esp_bt_hid_device_disconnect();
    if (err != ESP_OK) {
        connection_operation_set_state(CONNECTION_OP_SYNC_REARM_FAILED);
        portENTER_CRITICAL(&s_reconnect_mux);
        s_reconnect_paused = false;
        portEXIT_CRITICAL(&s_reconnect_mux);
        ESP_LOGE(TAG, "Could not disconnect HID host for SYNC: %s",
                 esp_err_to_name(err));
        (void)connection_operation_arm_watchdog();
        return err;
    }

    (void)connection_operation_arm_watchdog();
    ESP_LOGW(TAG, "HID_DISCONNECT_FOR_SYNC_REQUESTED; preserving the bonded host");
    return ESP_OK;
}

static esp_err_t request_saved_host_connection(const esp_bd_addr_t host)
{
    esp_bd_addr_t target;
    memcpy(target, host, sizeof(target));
    connection_operation_state_t state =
        connection_operation_snapshot(NULL, NULL);
    if (connection_operation_is_reconnect(state)) {
        connection_operation_set_state(CONNECTION_OP_RECONNECT_CONNECTING);
    } else {
        connection_operation_begin(CONNECTION_OP_RECONNECT_CONNECTING, target,
                                   false);
    }
    portENTER_CRITICAL(&s_reconnect_mux);
    s_reconnect_paused = false;
    s_reconnect_delay_ms = WIIMOTE_RECONNECT_INITIAL_DELAY_MS;
    portEXIT_CRITICAL(&s_reconnect_mux);
    wiimote_web_set_reconnect_pending(true);

    esp_err_t err = esp_bt_hid_device_connect(target);
    if (err != ESP_OK) {
        connection_operation_set_state(CONNECTION_OP_RECONNECT_FAILED);
        ESP_LOGW(TAG, "Saved-host reconnect request failed: %s",
                 esp_err_to_name(err));
        (void)connection_operation_arm_watchdog();
        return err;
    }

    char peer[18];
    ESP_LOGI(TAG, "MANUAL_HID_RECONNECT_REQUESTED peer=%s",
             wiimote_bt_address_string(target, peer));
    (void)connection_operation_arm_watchdog();
    return ESP_OK;
}

static esp_err_t request_saved_host_reconnect(void)
{
    if (sync_pairing_window_is_active()) return ESP_ERR_INVALID_STATE;
    connection_operation_state_t operation =
        connection_operation_snapshot(NULL, NULL);
    if (connection_operation_is_reconnect(operation)) {
        return resume_connection_operation();
    }
    if (operation != CONNECTION_OP_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_bd_addr_t host;
    if (!copy_last_host(host)) return ESP_ERR_NOT_FOUND;

    bool bonded = false;
    esp_err_t err = is_bonded_peer(host, &bonded);
    if (err != ESP_OK) return err;
    if (!bonded) {
        ESP_LOGE(TAG, "Saved host is not bonded; refusing Reconnect without Pair");
        return ESP_ERR_NOT_FOUND;
    }

    stop_hid_reconnect_timer();
    portENTER_CRITICAL(&s_reconnect_mux);
    s_reconnect_paused = true;
    portEXIT_CRITICAL(&s_reconnect_mux);
    connection_operation_begin(CONNECTION_OP_RECONNECT_RESTARTING_BLUEDROID,
                               host, false);
    wiimote_web_set_reconnect_pending(true);
    return request_bluetooth_stack_restart(
        CONNECTION_OP_RECONNECT_RESTARTING_BLUEDROID, host, false);
}
#endif

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
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            portENTER_CRITICAL(&s_reconnect_mux);
            s_reconnect_paused = false;
            s_reconnect_delay_ms = WIIMOTE_RECONNECT_INITIAL_DELAY_MS;
            portEXIT_CRITICAL(&s_reconnect_mux);
            if (!WIIMOTE_UPSTREAM_DISCOVERY_BASELINE) {
                close_sync_pairing_window("authentication complete");
            }
        } else if (!sync_pairing_window_is_active()) {
            stop_hid_reconnect_timer();
            portENTER_CRITICAL(&s_reconnect_mux);
            s_reconnect_paused = true;
            portEXIT_CRITICAL(&s_reconnect_mux);
            ESP_LOGW(TAG, "Pausing automatic HID reconnect after authentication failure");
        }
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin = {0};
        ESP_LOGI(TAG, "legacy PIN request min16=%u", param->pin_req.min_16_digit);
        bool sync_active = sync_pairing_window_is_active();
        if (!sync_active) {
            bool bonded = false;
            esp_err_t bond_err = is_bonded_peer(param->pin_req.bda, &bonded);
            if (bond_err != ESP_OK || !bonded) {
                ESP_LOGW(TAG, "Rejecting PIN request outside SYNC from an unverified peer");
                esp_err_t err = esp_bt_gap_pin_reply(param->pin_req.bda, false, 0, pin);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "PIN request rejection failed: %s", esp_err_to_name(err));
                }
                break;
            }
            ESP_LOGI(TAG, "Accepting legacy PIN request from a bonded host outside SYNC");
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
            ESP_LOGI(TAG, "legacy PIN reply result=%s mode=%s pin=reverse-host-address",
                     esp_err_to_name(err), sync_active ? "SYNC" : "bonded-reconnect");
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
#if CONFIG_EXAMPLE_WIFI_WEB_CONTROL
    case ESP_BT_GAP_REMOVE_BOND_DEV_COMPLETE_EVT: {
        char peer[18];
        ESP_LOGI(TAG, "bond removal result=%u peer=%s",
                 (unsigned)param->remove_bond_dev_cmpl.status,
                 wiimote_bt_address_string(param->remove_bond_dev_cmpl.bda, peer));
        if (stack_bond_remove_pending_for_peer(
                param->remove_bond_dev_cmpl.bda)) {
            s_pair_bond_remove_status =
                param->remove_bond_dev_cmpl.status;
            if (s_pair_bond_remove_done &&
                xSemaphoreGive(s_pair_bond_remove_done) != pdTRUE) {
                ESP_LOGE(TAG, "Could not signal Bluetooth bond-removal completion");
            }
        }
        break;
    }
#endif
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
    case ESP_HIDD_REGISTER_APP_EVT: {
        ESP_LOGI(TAG, "HID SDP registration result=%d", param->register_app.status);
        if (param->register_app.status != ESP_HIDD_SUCCESS) {
            ESP_LOGE(TAG, "HID app registration failed: status=%d",
                     param->register_app.status);
#if CONFIG_EXAMPLE_WIFI_WEB_CONTROL
            connection_operation_state_t operation =
                connection_operation_snapshot(NULL, NULL);
            if (operation == CONNECTION_OP_PAIR_RESTARTING_BLUEDROID) {
                connection_operation_set_state(CONNECTION_OP_PAIR_FAILED);
                (void)connection_operation_arm_watchdog();
            } else if (operation == CONNECTION_OP_RECONNECT_RESTARTING_BLUEDROID) {
                connection_operation_set_state(CONNECTION_OP_RECONNECT_FAILED);
                (void)connection_operation_arm_watchdog();
            }
#endif
            break;
        }
        if (!wiimote_sdp_hid_record_is_ready()) {
            ESP_LOGE(TAG, "HID registration did not produce a complete wiimote SDP record");
            esp_err_t err = esp_bt_hid_device_unregister_app();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Could not unregister incomplete HID app: %s",
                         esp_err_to_name(err));
            }
            break;
        }
        if (param->register_app.in_use) {
            char peer[18];
            save_last_host(param->register_app.bd_addr);
            ESP_LOGI(TAG, "HID virtual cable host restored peer=%s",
                     wiimote_bt_address_string(param->register_app.bd_addr, peer));
        }
#if CONFIG_EXAMPLE_WIFI_WEB_CONTROL
        connection_operation_state_t operation =
            connection_operation_snapshot(NULL, NULL);
        if (operation == CONNECTION_OP_PAIR_RESTARTING_BLUEDROID ||
            operation == CONNECTION_OP_RECONNECT_RESTARTING_BLUEDROID) {
            esp_err_t err = resume_connection_operation();
            if (err == ESP_ERR_INVALID_STATE) {
                (void)connection_operation_arm_watchdog();
            } else if (err != ESP_OK) {
                ESP_LOGE(TAG, "Could not resume operation after Bluetooth restart: %s",
                         esp_err_to_name(err));
                (void)connection_operation_arm_watchdog();
            }
            break;
        }
#endif
#if WIIMOTE_UPSTREAM_DISCOVERY_BASELINE
        maybe_arm_upstream_discovery("HID app ready");
#elif CONFIG_EXAMPLE_WIFI_WEB_CONTROL
        arm_bonded_host_reconnect("HID app ready");
        ESP_LOGI(TAG, "READY identity=Nintendo RVL-CNT-01; bonded hosts reconnect automatically; use web SYNC to pair a new host");
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
        esp_bd_addr_t saved_host;
        if (copy_last_host(saved_host)) schedule_hid_reconnect("HID app ready");
        break;
    }
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
            stop_hid_reconnect_timer();
            save_last_host(param->open.bd_addr);
            wiimote_state_open();
            wiimote_controls_on_connection_open();
            wiimote_memory_reset();
            ESP_ERROR_CHECK(wiimote_memory_start());
            wiimote_web_set_hid_connected(true);
            close_sync_pairing_window("HID connection opened");
            ESP_LOGI(TAG, "WIIREMOTE_HID_CONNECTED peer=%s",
                     wiimote_bt_address_string(param->open.bd_addr, peer));
            (void)wiimote_reports_send_current();

#if CONFIG_EXAMPLE_WIFI_WEB_CONTROL
            esp_bd_addr_t operation_host = {0};
            bool general_discoverable = false;
            connection_operation_state_t operation =
                connection_operation_snapshot(operation_host,
                                              &general_discoverable);
            if (connection_operation_is_pair(operation)) {
                if (operation == CONNECTION_OP_PAIR_OPENING_WINDOW) {
                    connection_operation_set_state(CONNECTION_OP_IDLE);
                } else if (operation == CONNECTION_OP_PAIR_RESTARTING_BLUEDROID) {
                    ESP_LOGW(TAG, "HID opened during Pair Bluetooth restart; waiting for the restart to finish");
                }
            } else if (operation == CONNECTION_OP_RECONNECT_CONNECTING) {
                connection_operation_set_state(CONNECTION_OP_IDLE);
            } else if (operation == CONNECTION_OP_RECONNECT_RESTARTING_BLUEDROID) {
                ESP_LOGW(TAG, "HID opened during Reconnect Bluetooth restart; waiting for the restart to finish");
            }
#endif
        }
        else if (param->open.status != ESP_HIDD_SUCCESS ||
                 param->open.conn_status == ESP_HIDD_CONN_STATE_DISCONNECTED) {
#if CONFIG_EXAMPLE_WIFI_WEB_CONTROL
            connection_operation_state_t operation =
                connection_operation_snapshot(NULL, NULL);
            if (operation == CONNECTION_OP_RECONNECT_CONNECTING ||
                operation == CONNECTION_OP_RECONNECT_RESTARTING_BLUEDROID) {
                connection_operation_set_state(CONNECTION_OP_RECONNECT_FAILED);
                (void)connection_operation_arm_watchdog();
                break;
            }
            if (connection_operation_is_pair(operation)) {
                connection_operation_set_state(CONNECTION_OP_PAIR_FAILED);
                (void)connection_operation_arm_watchdog();
                break;
            }
            if (operation == CONNECTION_OP_SYNC_REARM_DISCONNECTING) {
                connection_operation_set_state(CONNECTION_OP_SYNC_REARM_FAILED);
                (void)connection_operation_arm_watchdog();
                break;
            }
            if (operation == CONNECTION_OP_SYNC_REARM_FAILED) {
                portENTER_CRITICAL(&s_reconnect_mux);
                s_reconnect_paused = false;
                portEXIT_CRITICAL(&s_reconnect_mux);
            }
#endif
#if WIIMOTE_UPSTREAM_DISCOVERY_BASELINE
            maybe_arm_upstream_discovery("HID open failed");
#else
            arm_bonded_host_reconnect("HID open failed");
#endif
            schedule_hid_reconnect("HID open failed");
        }
        break;
    case ESP_HIDD_CLOSE_EVT: {
        ESP_LOGI(TAG, "HID close result=%d state=%d",
                 param->close.status, param->close.conn_status);
#if CONFIG_EXAMPLE_WIFI_WEB_CONTROL
        esp_bd_addr_t operation_host = {0};
        bool general_discoverable = false;
        connection_operation_state_t operation =
            connection_operation_snapshot(operation_host,
                                          &general_discoverable);
#endif
        if (param->close.status != ESP_HIDD_SUCCESS) {
#if CONFIG_EXAMPLE_WIFI_WEB_CONTROL
            if (param->close.status == ESP_HIDD_BUSY &&
                operation != CONNECTION_OP_IDLE) {
                ESP_LOGW(TAG, "HID close is busy; keeping operation phase=%s",
                         connection_operation_phase_name(operation));
                (void)connection_operation_arm_watchdog();
                break;
            }
            if (connection_operation_is_pair(operation)) {
                connection_operation_set_state(CONNECTION_OP_PAIR_FAILED);
            } else if (connection_operation_is_reconnect(operation)) {
                connection_operation_set_state(CONNECTION_OP_RECONNECT_FAILED);
            } else if (operation == CONNECTION_OP_SYNC_REARM_DISCONNECTING) {
                connection_operation_set_state(CONNECTION_OP_SYNC_REARM_FAILED);
            }
            if (operation != CONNECTION_OP_IDLE) {
                (void)connection_operation_arm_watchdog();
                break;
            }
#endif
            ESP_LOGE(TAG, "HID close failed; preserving current connection state");
            break;
        }
        if (param->close.conn_status != ESP_HIDD_CONN_STATE_DISCONNECTED) {
            ESP_LOGI(TAG, "HID disconnect not complete; waiting for final close callback");
#if CONFIG_EXAMPLE_WIFI_WEB_CONTROL
            if (operation != CONNECTION_OP_IDLE) {
                (void)connection_operation_arm_watchdog();
            }
#endif
            break;
        }
        wiimote_state_close();
        wiimote_web_set_hid_connected(false);
        close_sync_pairing_window("HID connection closed");
#if CONFIG_EXAMPLE_WIFI_WEB_CONTROL
        switch (operation) {
        case CONNECTION_OP_PAIR_RESTARTING_BLUEDROID:
        case CONNECTION_OP_RECONNECT_RESTARTING_BLUEDROID:
            ESP_LOGI(TAG, "HID closed during Bluetooth stack restart");
            break;
        case CONNECTION_OP_SYNC_REARM_DISCONNECTING: {
            esp_err_t err = start_sync_pairing_window(false);
            if (err == ESP_OK) {
                connection_operation_set_state(CONNECTION_OP_IDLE);
                ESP_LOGI(TAG, "SYNC window opened after controlled HID disconnect");
            } else {
                connection_operation_set_state(CONNECTION_OP_SYNC_REARM_FAILED);
                (void)connection_operation_arm_watchdog();
                ESP_LOGE(TAG, "Could not open SYNC after HID disconnect: %s",
                         esp_err_to_name(err));
            }
            break;
        }
        case CONNECTION_OP_PAIR_FAILED:
        case CONNECTION_OP_RECONNECT_FAILED:
        case CONNECTION_OP_SYNC_REARM_FAILED:
            (void)resume_connection_operation();
            break;
        case CONNECTION_OP_IDLE:
        case CONNECTION_OP_PAIR_OPENING_WINDOW:
        default:
            break;
        }
        if (operation != CONNECTION_OP_IDLE) break;
#endif
#if WIIMOTE_UPSTREAM_DISCOVERY_BASELINE
        maybe_arm_upstream_discovery("HID connection closed");
#else
        arm_bonded_host_reconnect("HID connection closed");
#endif
        portENTER_CRITICAL(&s_reconnect_mux);
        s_reconnect_delay_ms = WIIMOTE_RECONNECT_INCOMING_GRACE_MS;
        portEXIT_CRITICAL(&s_reconnect_mux);
        ESP_LOGI(TAG, "Waiting %u ms for an incoming HID reconnect before outgoing fallback",
                 (unsigned)WIIMOTE_RECONNECT_INCOMING_GRACE_MS);
        schedule_hid_reconnect("HID connection closed");
        break;
    }
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
    case ESP_HIDD_SEND_REPORT_EVT:
        if (param->send_report.status != ESP_HIDD_SUCCESS) {
            ESP_LOGW(TAG, "HID send id=0x%02x failed status=%d reason=%u",
                     param->send_report.report_id, param->send_report.status,
                     param->send_report.reason);
        }
        break;
    case ESP_HIDD_REPORT_ERR_EVT:
        ESP_LOGI(TAG, "HID handshake result=%d reason=%u",
                 param->report_err.status, param->report_err.reason);
        break;
    case ESP_HIDD_VC_UNPLUG_EVT: {
        ESP_LOGI(TAG, "virtual cable unplug result=%d state=%d",
                 param->vc_unplug.status, param->vc_unplug.conn_status);
#if CONFIG_EXAMPLE_WIFI_WEB_CONTROL
        connection_operation_state_t operation =
            connection_operation_snapshot(NULL, NULL);
        portENTER_CRITICAL(&s_reconnect_mux);
        bool pair_unplug_pending =
            operation == CONNECTION_OP_PAIR_RESTARTING_BLUEDROID &&
            s_pair_vc_unplug_pending;
        if (pair_unplug_pending) {
            s_pair_vc_unplug_status = param->vc_unplug.status;
            s_pair_vc_unplug_conn_status = param->vc_unplug.conn_status;
            s_pair_vc_unplug_pending = false;
        }
        portEXIT_CRITICAL(&s_reconnect_mux);
        if (pair_unplug_pending) {
            if (xSemaphoreGive(s_pair_vc_unplug_done) != pdTRUE) {
                ESP_LOGE(TAG, "Could not signal Pair virtual-cable-unplug completion");
            }
            break;
        }
        if (operation != CONNECTION_OP_IDLE) {
            ESP_LOGI(TAG, "Ignoring virtual-cable-unplug callback during operation=%s",
                     connection_operation_phase_name(operation));
            break;
        }
#endif
        if (param->vc_unplug.status != ESP_HIDD_SUCCESS ||
            param->vc_unplug.conn_status != ESP_HIDD_CONN_STATE_DISCONNECTED) {
            portENTER_CRITICAL(&s_reconnect_mux);
            s_reconnect_paused = false;
            portEXIT_CRITICAL(&s_reconnect_mux);
            ESP_LOGE(TAG, "Virtual-cable unplug did not complete; keeping saved HID host");
            schedule_hid_reconnect("virtual-cable unplug failed");
            break;
        }
        wiimote_state_close();
        wiimote_web_set_hid_connected(false);
        stop_hid_reconnect_timer();
        close_sync_pairing_window("virtual cable unplugged");
        clear_last_host();
#if WIIMOTE_UPSTREAM_DISCOVERY_BASELINE
        maybe_arm_upstream_discovery("virtual cable unplugged");
#else
        arm_bonded_host_reconnect("virtual cable unplugged");
#endif
        break;
    }
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
    load_last_host();
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

    const esp_timer_create_args_t reconnect_timer_args = {
        .callback = hid_reconnect_timeout,
        .name = "wiimote_reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&reconnect_timer_args, &s_reconnect_timer));

#if CONFIG_EXAMPLE_WIFI_WEB_CONTROL
    const esp_timer_create_args_t sync_timer_args = {
        .callback = sync_pairing_timeout,
        .name = "wiimote_sync",
    };
    ESP_ERROR_CHECK(esp_timer_create(&sync_timer_args, &s_sync_pairing_timer));
    s_pair_bond_remove_done = xSemaphoreCreateBinary();
    if (!s_pair_bond_remove_done) ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    s_pair_vc_unplug_done = xSemaphoreCreateBinary();
    if (!s_pair_vc_unplug_done) ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
#if CONFIG_BT_SDP_COMMON_ENABLED
    s_pair_sdp_deinit_done = xSemaphoreCreateBinary();
    if (!s_pair_sdp_deinit_done) ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
#endif
    const esp_timer_create_args_t connection_operation_timer_args = {
        .callback = connection_operation_timeout,
        .name = "connection_op",
    };
    ESP_ERROR_CHECK(esp_timer_create(&connection_operation_timer_args,
                                     &s_connection_operation_timer));
    wiimote_web_set_sync_callback(request_sync_with_host_switch);
    wiimote_web_set_reconnect_callback(request_saved_host_reconnect);
    wiimote_web_set_sync_rearm_callback(request_sync_after_disconnect);
#if CONFIG_EXAMPLE_BT_DISCOVERY_TRACE
    wiimote_web_set_virtual_cable_unplug_callback(request_virtual_cable_unplug);
#endif
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
#if NEXTMOTE_ENABLE_IMU_MOTION
#if NEXTMOTE_SEND_IMU_TO_HID
    ESP_ERROR_CHECK(wiimote_reports_start());
#else
    ESP_LOGW(TAG, "IMU-to-HID disabled (NEXTMOTE_SEND_IMU_TO_HID=0): web-only IMU, neutral HID accel");
#endif
    esp_err_t imu_err = imu_service_start();
    if (imu_err != ESP_OK) {
        ESP_LOGW(TAG, "BNO08x orientation/acceleration service unavailable: %s",
                 esp_err_to_name(imu_err));
    }
#else
    ESP_LOGW(TAG, "IMU motion disabled (NEXTMOTE_ENABLE_IMU_MOTION=0): no BNO08x I2C, no sensor HID task");
#endif
    ESP_ERROR_CHECK(wiimote_controls_start_fake_demo());
}

#include "wiimote_discovery.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hci/hci_layer.h"
#include "osi/allocator.h"
#include "stack/hcimsgs.h"
#include "wiimote_inquiry_policy.h"

#define IAC_ROTATION_INTERVAL_US 640000ULL
#define IAC_LOCK_TIMEOUT pdMS_TO_TICKS(500)

static const char *TAG = "wiimote_discovery";
static SemaphoreHandle_t s_capacity_done;
static SemaphoreHandle_t s_iac_lock;
static esp_timer_handle_t s_iac_timer;
static uint8_t s_capacity;
static esp_err_t s_capacity_result = ESP_ERR_INVALID_STATE;
static bool s_rotating;
static bool s_codes_ready;
static bool s_next_general;
static wiimote_discovery_error_callback_t s_error_callback;

BOOLEAN __real_btsnd_hcic_write_cur_iac_lap(UINT8 count, LAP *const codes);

static void capacity_complete(BT_HDR *response, void *context)
{
    (void)context;
    uint8_t capacity = 0;
    if (wiimote_inquiry_parse_capacity(response->data + response->offset,
                                       response->len, &capacity)) {
        s_capacity = capacity;
        s_capacity_result = ESP_OK;
    } else {
        ESP_LOGE(TAG, "Invalid controller Read Number of Supported IAC response");
        s_capacity_result = ESP_ERR_INVALID_RESPONSE;
    }
    osi_free(response);
    if (xSemaphoreGive(s_capacity_done) != pdTRUE) {
        ESP_LOGE(TAG, "Could not signal completion of the controller capacity query");
    }
}

static void rotation_tick(void *arg)
{
    (void)arg;
    if (xSemaphoreTake(s_iac_lock, IAC_LOCK_TIMEOUT) != pdTRUE) {
        ESP_LOGE(TAG, "Inquiry rotation could not acquire its configuration lock");
        s_error_callback(ESP_ERR_TIMEOUT);
        return;
    }
    if (!s_rotating || !s_codes_ready) {
        xSemaphoreGive(s_iac_lock);
        return;
    }

    LAP code;
    wiimote_inquiry_get_code(s_next_general, code);
    bool general = s_next_general;
    /* Keep page scan active while rearming inquiry scan for the next IAC. */
    BOOLEAN queued = btsnd_hcic_write_scan_enable(HCI_PAGE_SCAN_ENABLED);
    if (queued) queued = __real_btsnd_hcic_write_cur_iac_lap(1, &code);
    if (queued) {
        queued = btsnd_hcic_write_scan_enable(
            HCI_PAGE_SCAN_ENABLED | HCI_INQUIRY_SCAN_ENABLED);
    }
    if (queued) s_next_general = !s_next_general;
    else s_rotating = false;
    xSemaphoreGive(s_iac_lock);

    if (!queued) {
        ESP_LOGE(TAG, "Could not queue inquiry scan refresh for the next access code");
        s_error_callback(ESP_ERR_NO_MEM);
        return;
    }
    ESP_LOGI(TAG, "SYNC inquiry code=%s count=1", general ? "GIAC" : "LIAC");
}

BOOLEAN __wrap_btsnd_hcic_write_cur_iac_lap(UINT8 count, LAP *const codes)
{
    if (!s_iac_lock || !s_capacity) {
        ESP_LOGE(TAG, "Inquiry configuration requested before controller capacity was read");
        return FALSE;
    }
    if (xSemaphoreTake(s_iac_lock, IAC_LOCK_TIMEOUT) != pdTRUE) {
        ESP_LOGE(TAG, "Could not lock inquiry configuration");
        return FALSE;
    }

    LAP selected[2];
    uint8_t selected_count = 0;
    bool valid = wiimote_inquiry_select_codes(s_capacity, count, codes,
                                              selected, &selected_count);
    if (!valid) {
        xSemaphoreGive(s_iac_lock);
        ESP_LOGE(TAG, "Unsupported inquiry configuration: requested=%u supported=%u",
                 (unsigned)count, (unsigned)s_capacity);
        return FALSE;
    }

    BOOLEAN queued = __real_btsnd_hcic_write_cur_iac_lap(selected_count, selected);
    s_codes_ready = queued && count == 2;
    LAP general_code;
    wiimote_inquiry_get_code(true, general_code);
    s_next_general = memcmp(selected[0], general_code, sizeof(LAP)) != 0;
    xSemaphoreGive(s_iac_lock);
    if (!queued) {
        ESP_LOGE(TAG, "Could not queue inquiry configuration");
        return FALSE;
    }
    if (selected_count != count) {
        ESP_LOGI(TAG, "Controller supports one IAC; starting GIAC, rotating GIAC/LIAC during SYNC");
    }
    return TRUE;
}

esp_err_t wiimote_discovery_init(wiimote_discovery_error_callback_t error_callback)
{
    if (!error_callback || s_iac_lock) {
        ESP_LOGE(TAG, "Invalid or repeated discovery initialization");
        return ESP_ERR_INVALID_STATE;
    }
    s_error_callback = error_callback;
    s_iac_lock = xSemaphoreCreateMutex();
    s_capacity_done = xSemaphoreCreateBinary();
    if (!s_iac_lock || !s_capacity_done) {
        ESP_LOGE(TAG, "Could not allocate discovery synchronization objects");
        return ESP_ERR_NO_MEM;
    }

    BT_HDR *command = HCI_GET_CMD_BUF(0);
    if (!command) {
        ESP_LOGE(TAG, "Could not allocate controller capacity query");
        return ESP_ERR_NO_MEM;
    }
    command->data[0] = (uint8_t)HCI_READ_NUM_SUPPORTED_IAC;
    command->data[1] = (uint8_t)(HCI_READ_NUM_SUPPORTED_IAC >> 8);
    command->data[2] = 0;
    hci_cmd_metadata_t *metadata = HCI_GET_CMD_METAMSG(command);
    metadata->opcode = HCI_READ_NUM_SUPPORTED_IAC;
    metadata->command_complete_cb = capacity_complete;
    hci_layer_get_interface()->transmit_command(command, capacity_complete, NULL, NULL);
    if (xSemaphoreTake(s_capacity_done, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGE(TAG, "Controller inquiry-capacity query timed out");
        return ESP_ERR_TIMEOUT;
    }
    if (s_capacity_result != ESP_OK) return s_capacity_result;

    const esp_timer_create_args_t timer_args = {
        .callback = rotation_tick,
        .name = "wiimote_iac",
    };
    esp_err_t err = esp_timer_create(&timer_args, &s_iac_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not create inquiry rotation timer: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Controller supported inquiry access codes=%u", (unsigned)s_capacity);
#if WIIMOTE_DIAGNOSTIC_GIAC_ONLY
    ESP_LOGW(TAG, "DIAGNOSTIC GIAC-only hold enabled; Limited CoD retained, LIAC rotation disabled");
#endif
    return ESP_OK;
}

esp_err_t wiimote_discovery_set_rotation(bool enabled)
{
    if (!s_iac_timer) {
        ESP_LOGE(TAG, "Discovery rotation is not ready");
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_iac_lock, IAC_LOCK_TIMEOUT) != pdTRUE) {
        ESP_LOGE(TAG, "Discovery rotation lock timed out");
        return ESP_ERR_TIMEOUT;
    }
    s_rotating = false;
    esp_err_t err = ESP_OK;
    if (esp_timer_is_active(s_iac_timer)) {
        err = esp_timer_stop(s_iac_timer);
        if (err != ESP_OK) {
            xSemaphoreGive(s_iac_lock);
            ESP_LOGE(TAG, "Could not stop inquiry rotation: %s", esp_err_to_name(err));
            return err;
        }
    }
    if (!enabled) s_codes_ready = false;
    if (enabled && s_capacity == 1 && !WIIMOTE_DIAGNOSTIC_GIAC_ONLY) {
        s_rotating = true;
        err = esp_timer_start_periodic(s_iac_timer, IAC_ROTATION_INTERVAL_US);
        if (err != ESP_OK) s_rotating = false;
    }
    xSemaphoreGive(s_iac_lock);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not start inquiry rotation: %s", esp_err_to_name(err));
    }
    return err;
}

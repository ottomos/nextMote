#include "wiimote_sdp.h"

#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "stack/hidd_api.h"
#include "stack/sdp_api.h"
#include "wiimote_sdp_values.h"

static const char *TAG = "wiimote_sdp";
static portMUX_TYPE s_ready_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_hid_ready;

tHID_STATUS __real_HID_DevAddRecord(uint32_t handle, char *name,
                                   char *description, char *provider,
                                   uint16_t subclass, uint16_t descriptor_length,
                                   uint8_t *descriptor);

static void set_hid_ready(bool ready)
{
    portENTER_CRITICAL(&s_ready_mux);
    s_hid_ready = ready;
    portEXIT_CRITICAL(&s_ready_mux);
}

bool wiimote_sdp_hid_record_is_ready(void)
{
    portENTER_CRITICAL(&s_ready_mux);
    bool ready = s_hid_ready;
    portEXIT_CRITICAL(&s_ready_mux);
    return ready;
}

void wiimote_sdp_clear_hid_ready(void)
{
    set_hid_ready(false);
}

static esp_err_t apply_values(uint32_t handle, wiimote_sdp_values_t attributes)
{
    for (size_t i = 0; i < attributes.count; ++i) {
        const wiimote_sdp_value_t *attribute = &attributes.values[i];
        uint8_t bytes[2];
        uint8_t type;
        uint32_t length;
        switch (attribute->type) {
        case WIIMOTE_SDP_UINT8:
            if (attribute->value > UINT8_MAX) {
                ESP_LOGE(TAG, "Invalid uint8 SDP value at 0x%04x", attribute->id);
                return ESP_ERR_INVALID_ARG;
            }
            bytes[0] = (uint8_t)attribute->value;
            type = UINT_DESC_TYPE;
            length = 1;
            break;
        case WIIMOTE_SDP_UINT16:
            bytes[0] = (uint8_t)(attribute->value >> 8);
            bytes[1] = (uint8_t)attribute->value;
            type = UINT_DESC_TYPE;
            length = 2;
            break;
        case WIIMOTE_SDP_BOOLEAN:
            if (attribute->value > 1) {
                ESP_LOGE(TAG, "Invalid boolean SDP value at 0x%04x", attribute->id);
                return ESP_ERR_INVALID_ARG;
            }
            bytes[0] = (uint8_t)attribute->value;
            type = BOOLEAN_DESC_TYPE;
            length = 1;
            break;
        default:
            ESP_LOGE(TAG, "Unknown SDP value type at 0x%04x", attribute->id);
            return ESP_ERR_INVALID_ARG;
        }
        if (!SDP_AddAttribute(handle, attribute->id, type, length, bytes)) {
            ESP_LOGE(TAG, "Could not set SDP attribute 0x%04x on record 0x%08lx",
                     attribute->id, (unsigned long)handle);
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static esp_err_t apply_text(uint32_t handle, uint16_t id, char *text)
{
    if (!text) {
        ESP_LOGE(TAG, "Invalid SDP text at attribute 0x%04x", id);
        return ESP_ERR_INVALID_ARG;
    }
    size_t text_length = strlen(text);
    if (text_length > UINT16_MAX) {
        ESP_LOGE(TAG, "SDP text is too long at attribute 0x%04x", id);
        return ESP_ERR_INVALID_ARG;
    }
    /* SDP text contains the characters, not the C string's terminating NUL. */
    uint32_t length = (uint32_t)text_length;
    if (!SDP_AddAttribute(handle, id, TEXT_STR_DESC_TYPE, length, (uint8_t *)text)) {
        ESP_LOGE(TAG, "Could not set SDP text attribute 0x%04x", id);
        return ESP_FAIL;
    }
    return ESP_OK;
}

tHID_STATUS __wrap_HID_DevAddRecord(uint32_t handle, char *name,
                                   char *description, char *provider,
                                   uint16_t subclass, uint16_t descriptor_length,
                                   uint8_t *descriptor)
{
    set_hid_ready(false);
    tHID_STATUS status = __real_HID_DevAddRecord(
        handle, name, description, provider, subclass, descriptor_length, descriptor);
    if (status != HID_SUCCESS) {
        ESP_LOGE(TAG, "Generic HID SDP creation failed: status=%u", (unsigned)status);
        return status;
    }
    esp_err_t err = apply_values(handle, wiimote_sdp_hid_values());
    if (err == ESP_OK) err = apply_text(handle, ATTR_ID_SERVICE_NAME, name);
    if (err == ESP_OK) err = apply_text(handle, ATTR_ID_SERVICE_DESCRIPTION, description);
    if (err == ESP_OK) err = apply_text(handle, ATTR_ID_PROVIDER_NAME, provider);
    if (err != ESP_OK) return HID_ERR_NOT_REGISTERED;

    set_hid_ready(true);
    ESP_LOGI(TAG, "espMote HID SDP matches captured wiimote policy: handle=0x%08lx",
             (unsigned long)handle);
    return HID_SUCCESS;
}

esp_err_t wiimote_sdp_apply_device_id(uint32_t handle)
{
    if (!handle) {
        ESP_LOGE(TAG, "Invalid Device ID SDP record handle");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = apply_values(handle, wiimote_sdp_device_id_values());
    if (err != ESP_OK) return err;

    tSDP_PROTOCOL_ELEM protocols[2] = {
        {.protocol_uuid = UUID_PROTOCOL_L2CAP, .num_params = 1, .params = {0x0001}},
        {.protocol_uuid = UUID_PROTOCOL_SDP, .num_params = 0},
    };
    uint16_t browse_group = UUID_SERVCLASS_PUBLIC_BROWSE_GROUP;
    if (!SDP_AddProtocolList(handle, 2, protocols)) {
        ESP_LOGE(TAG, "Could not set wiimote Device ID protocol list");
        return ESP_FAIL;
    }
    if (!SDP_AddUuidSequence(handle, ATTR_ID_BROWSE_GROUP_LIST, 1, &browse_group)) {
        ESP_LOGE(TAG, "Could not set wiimote Device ID browse group");
        return ESP_FAIL;
    }
    if (!SDP_AddProfileDescriptorList(handle, 0x1200, 0x0100)) {
        ESP_LOGE(TAG, "Could not set wiimote Device ID profile descriptor");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "espMote Device ID SDP matches captured wiimote policy: handle=0x%08lx",
             (unsigned long)handle);
    return ESP_OK;
}

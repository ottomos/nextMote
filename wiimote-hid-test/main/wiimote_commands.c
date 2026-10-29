#include "wiimote_commands.h"

#include "esp_hidd_api.h"
#include "esp_log.h"
#include "wiimote_memory.h"
#include "wiimote_protocol.h"
#include "wiimote_reports.h"
#include "wiimote_state.h"

static const char *TAG = "wiimote_commands";

uint8_t wiimote_commands_process_output_report(uint8_t report_id,
                                              const uint8_t *payload,
                                              uint16_t length)
{
    ESP_LOGI(TAG, "output id=0x%02x length=%u", report_id, length);
    if (payload != NULL && length > 0) ESP_LOG_BUFFER_HEX(TAG, payload, length);

    switch (report_id) {
    case WIIMOTE_REPORT_LED:
        if (length != 1) return ESP_HID_PAR_HANDSHAKE_RSP_ERR_INVALID_PARAM;
        wiimote_state_set_leds(payload[0]);
        ESP_LOGI(TAG, "LED state=0x%02x", payload[0] & 0xf0U);
        return ESP_HID_PAR_HANDSHAKE_RSP_SUCCESS;
    case WIIMOTE_REPORT_IR_ENABLE:
    case WIIMOTE_REPORT_IR_ENABLE_2: {
        if (length != 1) return ESP_HID_PAR_HANDSHAKE_RSP_ERR_INVALID_PARAM;
        bool enabled = (payload[0] & 0x04U) != 0;
        wiimote_state_set_ir_feature(report_id, enabled);
        ESP_LOGI(TAG, "IR feature id=0x%02x enable=%u status_flag=%u (synthetic data)",
                 report_id, enabled, wiimote_state_ir_enabled());
        wiimote_state_snapshot_t state;
        wiimote_state_get_snapshot(&state);
        if (state.report_mode == WIIMOTE_REPORT_BUTTONS_ACCEL_IR &&
            state.ir_logic_enabled && state.ir_sensor_enabled) {
            (void)wiimote_reports_send_current();
        }
        return ESP_HID_PAR_HANDSHAKE_RSP_SUCCESS;
    }
    case WIIMOTE_REPORT_SPEAKER_ENABLE:
    case WIIMOTE_REPORT_SPEAKER_MUTE:
        if (length != 1) return ESP_HID_PAR_HANDSHAKE_RSP_ERR_INVALID_PARAM;
        ESP_LOGI(TAG, "Audio feature id=0x%02x enable=%u (audio hardware absent)",
                 report_id, (payload[0] & 0x04U) != 0);
        return ESP_HID_PAR_HANDSHAKE_RSP_SUCCESS;
    case 0x10:
        if (length != 1) return ESP_HID_PAR_HANDSHAKE_RSP_ERR_INVALID_PARAM;
        ESP_LOGI(TAG, "Rumble state=%u (motor hardware absent)", payload[0] & 1U);
        return ESP_HID_PAR_HANDSHAKE_RSP_SUCCESS;
    case WIIMOTE_REPORT_STATUS_REQUEST:
        if (length != 1) return ESP_HID_PAR_HANDSHAKE_RSP_ERR_INVALID_PARAM;
        (void)wiimote_reports_send_status();
        return ESP_HID_PAR_HANDSHAKE_RSP_SUCCESS;
    case WIIMOTE_REPORT_MODE: {
        if (length != 2) return ESP_HID_PAR_HANDSHAKE_RSP_ERR_INVALID_PARAM;
        uint8_t report_mode = payload[1];
        bool continuous = (payload[0] & 0x04U) != 0;
        wiimote_state_set_report_mode(report_mode, continuous);
        ESP_LOGI(TAG, "report mode=0x%02x flags=0x%02x (continuous=%d supported=%d)",
                 report_mode, payload[0], continuous,
                 wiimote_protocol_is_data_report(report_mode));
        return wiimote_reports_send_current();
    }
    case WIIMOTE_REPORT_READ_MEMORY:
        return wiimote_memory_handle_read(payload, length);
    case WIIMOTE_REPORT_WRITE_MEMORY:
        return wiimote_memory_handle_write(payload, length);
    default:
        ESP_LOGW(TAG, "Unsupported Wii Remote output report 0x%02x", report_id);
        return ESP_HID_PAR_HANDSHAKE_RSP_ERR_UNSUPPORTED_REQ;
    }
}

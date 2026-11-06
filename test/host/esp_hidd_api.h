#pragma once
#include <stdint.h>
#include "esp_err.h"
#define ESP_HIDD_REPORT_TYPE_INTRDATA 0
#define ESP_HID_PAR_HANDSHAKE_RSP_SUCCESS 0
#define ESP_HID_PAR_HANDSHAKE_RSP_NOT_READY 1
#define ESP_HID_PAR_HANDSHAKE_RSP_ERR_UNSUPPORTED_REQ 3
esp_err_t esp_bt_hid_device_send_report(int type, uint8_t id, uint16_t length,
                                       uint8_t *payload);

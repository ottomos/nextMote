#pragma once

#include <stdint.h>

#include "ir_camera.h"
#include "esp_err.h"

esp_err_t wiimote_reports_start(void);
void wiimote_reports_set_camera_state(const ir_camera_state_t *state);
uint8_t wiimote_reports_send_input(uint8_t report_id, const uint8_t *data, uint16_t length);
uint8_t wiimote_reports_send_data(uint8_t report_id);
uint8_t wiimote_reports_send_current(void);
uint8_t wiimote_reports_send_status(void);
void wiimote_reports_send_interrupt_ack(uint8_t report_id, uint8_t error);
void wiimote_reports_send_memory_reply(uint8_t error, uint16_t address,
                                       const uint8_t *data, uint8_t length);
void wiimote_reports_send_memory_error(uint8_t error, uint16_t address);
void wiimote_reports_send_memory_write_ack(uint8_t error);

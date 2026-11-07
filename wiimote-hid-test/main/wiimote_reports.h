#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ir_camera.h"
#include "esp_err.h"

esp_err_t wiimote_reports_start(void);
/* Data reports accepted by the HID stack, and those it later failed to send. */
void wiimote_reports_get_counters(uint32_t *data_sent, uint32_t *send_failures);
void wiimote_reports_note_send_failure(void);
void wiimote_reports_set_camera_state(const ir_camera_state_t *state);
uint8_t wiimote_reports_send_input(uint8_t report_id, const uint8_t *data, uint16_t length);
uint8_t wiimote_reports_send_data(uint8_t report_id);
uint8_t wiimote_reports_send_current(void);
uint8_t wiimote_reports_send_status(void);
/* Plugs or unplugs the emulated Nunchuk; returns true when the state changed. */
bool wiimote_reports_set_nunchuk_attached(bool attached);
void wiimote_reports_send_interrupt_ack(uint8_t report_id, uint8_t error);
void wiimote_reports_send_memory_reply(uint8_t error, uint16_t address,
                                       const uint8_t *data, uint8_t length);
void wiimote_reports_send_memory_error(uint8_t error, uint16_t address);
void wiimote_reports_send_memory_write_ack(uint8_t error);

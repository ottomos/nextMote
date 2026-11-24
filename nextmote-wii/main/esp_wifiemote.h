#include <stddef.h>
#include <stdint.h>
#pragma once

#include <stddef.h>
#include <stdint.h>

extern uint8_t wiimote_report_descriptor[];
extern const size_t wiimote_report_descriptor_len;

const char *wiimote_bt_address_string(uint8_t address[6], char out[18]);

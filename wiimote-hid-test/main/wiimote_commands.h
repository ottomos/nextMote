#pragma once

#include <stdint.h>

uint8_t wiimote_commands_process_output_report(uint8_t report_id,
                                              const uint8_t *payload,
                                              uint16_t length);

#pragma once

#include "esp_err.h"

void wiimote_controls_init(void);
void wiimote_controls_on_connection_open(void);
esp_err_t wiimote_controls_start_fake_demo(void);

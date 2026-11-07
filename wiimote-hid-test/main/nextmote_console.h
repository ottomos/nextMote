#pragma once

#include <stdbool.h>

/*
 * Line commands on the USB serial console (115200 8N1, end with Enter):
 *   status  - show mode and stored network name
 *   setup   - reboot into Wi-Fi setup mode once (stored network is kept)
 *   normal  - reboot into normal mode (needs a stored network)
 *   forget  - erase the stored network and reboot (factory Wi-Fi state)
 *   reboot  - restart
 */
void nextmote_console_start(bool setup_mode);

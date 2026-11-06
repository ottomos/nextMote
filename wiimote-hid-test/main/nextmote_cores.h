#pragma once

/*
 * Core layout: core 0 runs Bluetooth, Wi-Fi/TCP-IP and Wii protocol helpers;
 * core 1 runs sensors, the 20 ms HID sensor stream, controls and the web server.
 */
#define NEXTMOTE_COMM_CORE 0
#define NEXTMOTE_APP_CORE 1

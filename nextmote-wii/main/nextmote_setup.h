#pragma once

/*
 * Setup mode: nextMote runs only its own open Wi-Fi network
 * ("nextMote-Setup-XXXX", http://192.168.4.1/) with a captive DNS and a page
 * to choose the home network. Bluetooth, sensors and the controller page are
 * not started. A successful connection test saves the credentials and reboots
 * into normal mode. Returns once the network, DNS and web server are running.
 */
void nextmote_setup_run(void);

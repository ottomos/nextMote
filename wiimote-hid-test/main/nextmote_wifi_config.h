#pragma once

#include <stdbool.h>

#include "esp_err.h"

#define NEXTMOTE_WIFI_SSID_MAX 32
#define NEXTMOTE_WIFI_PASSWORD_MAX 64

/*
 * Wi-Fi station credentials and the setup-mode request flag, kept in NVS
 * namespace "netcfg". Factory state (no stored SSID) boots into setup mode.
 */

/* Loads stored credentials; buffers must hold MAX + 1 bytes. */
bool nextmote_wifi_config_load(char *ssid, char *password);
esp_err_t nextmote_wifi_config_save(const char *ssid, const char *password);
esp_err_t nextmote_wifi_config_forget(void);

/* One-shot flag: the next boot enters setup mode even with stored credentials. */
esp_err_t nextmote_wifi_config_request_setup(void);
esp_err_t nextmote_wifi_config_clear_setup_request(void);

/* Called once at boot: true if setup mode must run (no credentials, or a
 * pending request, which is consumed so a later reset returns to normal). */
bool nextmote_wifi_config_boot_into_setup(void);

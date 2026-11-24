#include "nextmote_wifi_config.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "wifi_config";
static const char *NS = "netcfg";

bool nextmote_wifi_config_load(char *ssid, char *password)
{
    ssid[0] = '\0';
    password[0] = '\0';
    nvs_handle_t nvs;
    if (nvs_open(NS, NVS_READONLY, &nvs) != ESP_OK) return false;
    size_t ssid_len = NEXTMOTE_WIFI_SSID_MAX + 1;
    size_t pass_len = NEXTMOTE_WIFI_PASSWORD_MAX + 1;
    esp_err_t err = nvs_get_str(nvs, "ssid", ssid, &ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_str(nvs, "pass", password, &pass_len);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            password[0] = '\0';
            err = ESP_OK;
        }
    }
    nvs_close(nvs);
    if (err != ESP_OK || ssid[0] == '\0') {
        ssid[0] = '\0';
        password[0] = '\0';
        return false;
    }
    return true;
}

esp_err_t nextmote_wifi_config_save(const char *ssid, const char *password)
{
    if (!ssid || !password || ssid[0] == '\0' || strlen(ssid) > NEXTMOTE_WIFI_SSID_MAX ||
        strlen(password) > NEXTMOTE_WIFI_PASSWORD_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_str(nvs, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(nvs, "pass", password);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "Saved Wi-Fi network \"%s\": %s", ssid, esp_err_to_name(err));
    return err;
}

esp_err_t nextmote_wifi_config_forget(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(nvs);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGW(TAG, "Forgot stored Wi-Fi settings: %s", esp_err_to_name(err));
    return err;
}

static esp_err_t set_setup_flag(uint8_t value)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    if (value) {
        err = nvs_set_u8(nvs, "setup", value);
    } else {
        err = nvs_erase_key(nvs, "setup");
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

esp_err_t nextmote_wifi_config_request_setup(void)
{
    esp_err_t err = set_setup_flag(1);
    ESP_LOGW(TAG, "Setup mode requested for next boot: %s", esp_err_to_name(err));
    return err;
}

esp_err_t nextmote_wifi_config_clear_setup_request(void)
{
    return set_setup_flag(0);
}

bool nextmote_wifi_config_boot_into_setup(void)
{
    bool requested = false;
    nvs_handle_t nvs;
    if (nvs_open(NS, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t flag = 0;
        requested = nvs_get_u8(nvs, "setup", &flag) == ESP_OK && flag != 0;
        nvs_close(nvs);
    }
    if (requested) {
        (void)nextmote_wifi_config_clear_setup_request();
        ESP_LOGW(TAG, "Setup mode: requested (one-shot)");
        return true;
    }
    char ssid[NEXTMOTE_WIFI_SSID_MAX + 1];
    char password[NEXTMOTE_WIFI_PASSWORD_MAX + 1];
    const bool have = nextmote_wifi_config_load(ssid, password);
    memset(password, 0, sizeof(password));
    if (!have) ESP_LOGW(TAG, "Setup mode: no stored Wi-Fi network (factory state)");
    return !have;
}

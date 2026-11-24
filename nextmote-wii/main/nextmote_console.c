#include "nextmote_console.h"

#include <ctype.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nextmote_wifi_config.h"

#define CONSOLE_UART UART_NUM_0
#define CONSOLE_LINE_MAX 64

static const char *TAG = "console";
static bool s_setup_mode;

static void restart_soon(void)
{
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

static void handle_line(char *line)
{
    while (*line && isspace((unsigned char)*line)) ++line;
    size_t len = strlen(line);
    while (len && isspace((unsigned char)line[len - 1])) line[--len] = '\0';
    if (len == 0) return;

    char ssid[NEXTMOTE_WIFI_SSID_MAX + 1];
    char password[NEXTMOTE_WIFI_PASSWORD_MAX + 1];
    const bool have = nextmote_wifi_config_load(ssid, password);
    memset(password, 0, sizeof(password));

    if (strcmp(line, "status") == 0) {
        ESP_LOGI(TAG, "mode=%s stored_network=%s%s%s", s_setup_mode ? "setup" : "normal",
                 have ? "\"" : "", have ? ssid : "none", have ? "\"" : "");
    } else if (strcmp(line, "setup") == 0) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(nextmote_wifi_config_request_setup());
        ESP_LOGW(TAG, "Rebooting into setup mode");
        restart_soon();
    } else if (strcmp(line, "normal") == 0) {
        if (!have) {
            ESP_LOGW(TAG, "No stored network; staying in setup mode");
            return;
        }
        (void)nextmote_wifi_config_clear_setup_request();
        ESP_LOGW(TAG, "Rebooting into normal mode");
        restart_soon();
    } else if (strcmp(line, "forget") == 0) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(nextmote_wifi_config_forget());
        restart_soon();
    } else if (strcmp(line, "reboot") == 0) {
        restart_soon();
    } else {
        ESP_LOGI(TAG, "Commands: status, setup, normal, forget, reboot");
    }
}

static void console_task(void *arg)
{
    (void)arg;
    char line[CONSOLE_LINE_MAX];
    size_t len = 0;
    bool overflow = false;
    for (;;) {
        uint8_t c;
        if (uart_read_bytes(CONSOLE_UART, &c, 1, portMAX_DELAY) != 1) continue;
        if (c == '\r' || c == '\n') {
            line[len] = '\0';
            if (!overflow) handle_line(line);
            len = 0;
            overflow = false;
        } else if (len + 1 < sizeof(line)) {
            line[len++] = (char)c;
        } else {
            overflow = true;
        }
    }
}

void nextmote_console_start(bool setup_mode)
{
    s_setup_mode = setup_mode;
    if (!uart_is_driver_installed(CONSOLE_UART)) {
        esp_err_t err = uart_driver_install(CONSOLE_UART, 256, 0, 0, NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Serial console unavailable: %s", esp_err_to_name(err));
            return;
        }
    }
    xTaskCreate(console_task, "console", 3072, NULL, 3, NULL);
    ESP_LOGI(TAG, "Serial commands: status, setup, normal, forget, reboot");
}

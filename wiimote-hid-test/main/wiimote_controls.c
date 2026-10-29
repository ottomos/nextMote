#include "wiimote_controls.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "wiimote_protocol.h"
#include "wiimote_reports.h"
#include "wiimote_state.h"
#include "wiimote_web.h"

#if CONFIG_EXAMPLE_FAKE_BUTTON_DEMO
#define BUTTON_DEMO_PRESS_MS 500
#define BUTTON_DEMO_RELEASE_MS 250
static const char *TAG = "wiimote_controls";
#endif

static bool s_web_control_active;
static portMUX_TYPE s_controls_mux = portMUX_INITIALIZER_UNLOCKED;

static void set_web_button_state(uint16_t button_mask)
{
    portENTER_CRITICAL(&s_controls_mux);
    s_web_control_active = true;
    portEXIT_CRITICAL(&s_controls_mux);

    if (wiimote_state_set_button_mask(button_mask))
        (void)wiimote_reports_send_current();
}

void wiimote_controls_init(void)
{
    wiimote_web_set_button_callback(set_web_button_state);
}

void wiimote_controls_on_connection_open(void)
{
    portENTER_CRITICAL(&s_controls_mux);
    s_web_control_active = false;
    portEXIT_CRITICAL(&s_controls_mux);
}

#if CONFIG_EXAMPLE_FAKE_BUTTON_DEMO
static const struct {
    const char *name;
    uint16_t mask;
} s_button_sequence[] = {
    {"A", 0x0008}, {"B", 0x0004}, {"ONE", 0x0002}, {"TWO", 0x0001},
    {"UP", 0x0800}, {"DOWN", 0x0400}, {"LEFT", 0x0100}, {"RIGHT", 0x0200},
    {"PLUS", 0x1000}, {"MINUS", 0x0010}, {"HOME", 0x0080},
};

static void fake_button_task(void *arg)
{
    (void)arg;
    size_t index = 0;
    ESP_LOGI(TAG, "Fake button sequence ready");

    for (;;) {
        wiimote_state_snapshot_t state;
        wiimote_state_get_snapshot(&state);

        portENTER_CRITICAL(&s_controls_mux);
        bool web_control_active = s_web_control_active;
        portEXIT_CRITICAL(&s_controls_mux);

        if (!state.connected ||
            !wiimote_protocol_is_data_report(state.report_mode) || web_control_active) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        wiimote_state_set_button_bytes((uint8_t)s_button_sequence[index].mask,
                                       (uint8_t)(s_button_sequence[index].mask >> 8));
        ESP_LOGI(TAG, "FAKE_BUTTON press %s (continuous=%d)",
                 s_button_sequence[index].name, state.continuous_reporting);
        (void)wiimote_reports_send_current();
        vTaskDelay(pdMS_TO_TICKS(BUTTON_DEMO_PRESS_MS));

        wiimote_state_set_button_bytes(0, 0);
        ESP_LOGI(TAG, "FAKE_BUTTON release %s", s_button_sequence[index].name);
        if (state.continuous_reporting) (void)wiimote_reports_send_current();
        vTaskDelay(pdMS_TO_TICKS(BUTTON_DEMO_RELEASE_MS));

        index = (index + 1) % (sizeof(s_button_sequence) / sizeof(s_button_sequence[0]));
    }
}
#endif

esp_err_t wiimote_controls_start_fake_demo(void)
{
#if CONFIG_EXAMPLE_FAKE_BUTTON_DEMO
    BaseType_t result = xTaskCreate(fake_button_task, "fake_button_demo", 4096,
                                    NULL, 5, NULL);
    if (result != pdPASS) return ESP_ERR_NO_MEM;
#endif
    return ESP_OK;
}

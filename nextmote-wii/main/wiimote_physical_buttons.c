#include "wiimote_physical_buttons.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static wiimote_physical_buttons_snapshot_t s_snapshot;
static portMUX_TYPE s_snapshot_mux = portMUX_INITIALIZER_UNLOCKED;

void wiimote_physical_buttons_get_snapshot(wiimote_physical_buttons_snapshot_t *snapshot)
{
    portENTER_CRITICAL(&s_snapshot_mux);
    *snapshot = s_snapshot;
    portEXIT_CRITICAL(&s_snapshot_mux);
}

#ifndef NEXTMOTE_ENABLE_PHYSICAL_BUTTONS
#define NEXTMOTE_ENABLE_PHYSICAL_BUTTONS 0
#endif

#if NEXTMOTE_ENABLE_PHYSICAL_BUTTONS
#include "driver/gpio.h"
#include "esp_hidd_api.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wiimote_button_input.h"
#include "wiimote_reports.h"
#include "wiimote_state.h"

#if CONFIG_EXAMPLE_FAKE_BUTTON_DEMO
#error "Disable the fake button demo when physical buttons are enabled"
#endif

static const char *TAG = "physical_buttons";
/* Shield A/B/C/D/F/E: Up/Right/Down/Left/Wii A/Wii B. */
static const gpio_num_t s_pins[WIIMOTE_PHYSICAL_BUTTON_COUNT] = {
    GPIO_NUM_32, GPIO_NUM_33, GPIO_NUM_25, GPIO_NUM_14, GPIO_NUM_23, GPIO_NUM_19,
};

static void button_task(void *arg)
{
    (void)arg;
    wiimote_button_debounce_t buttons[WIIMOTE_PHYSICAL_BUTTON_COUNT] = {0};
    bool pending = false;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        uint8_t pressed_bits = 0;
        bool changed = false;
        for (unsigned i = 0; i < WIIMOTE_PHYSICAL_BUTTON_COUNT; ++i) {
            const bool transition = wiimote_button_debounce_update(&buttons[i],
                                                      gpio_get_level(s_pins[i]) == 0, now_ms);
            changed |= transition;
            if (transition) {
                portENTER_CRITICAL(&s_snapshot_mux);
                if (buttons[i].pressed) ++s_snapshot.presses[i];
                else ++s_snapshot.releases[i];
                portEXIT_CRITICAL(&s_snapshot_mux);
            }
            if (buttons[i].pressed) pressed_bits |= (uint8_t)(1U << i);
        }
        const uint16_t mask = wiimote_physical_button_mask(pressed_bits);
        portENTER_CRITICAL(&s_snapshot_mux);
        s_snapshot.mask = mask;
        portEXIT_CRITICAL(&s_snapshot_mux);
        if (changed) {
            pending = true;
            ESP_LOGI(TAG, "Physical mask=0x%04x", mask);
        }
        /* Keep held inputs available across Bluetooth connection resets. */
        const bool can_send = wiimote_state_set_physical_button_mask(mask);
        if (pending && can_send) {
            const uint8_t result = wiimote_reports_send_current();
            if (result == ESP_HID_PAR_HANDSHAKE_RSP_SUCCESS) {
                pending = false;
            } else {
                ESP_LOGW(TAG, "Button report failed: %d", result);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(10) ? pdMS_TO_TICKS(10) : 1);
    }
}
#endif

esp_err_t wiimote_physical_buttons_start(void)
{
#if NEXTMOTE_ENABLE_PHYSICAL_BUTTONS
    uint64_t pin_mask = 0;
    for (unsigned i = 0; i < WIIMOTE_PHYSICAL_BUTTON_COUNT; ++i) {
        pin_mask |= 1ULL << s_pins[i];
    }
    const gpio_config_t config = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) return err;
    if (xTaskCreate(button_task, "physical_buttons", 3072, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not create physical-button task");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Shield buttons enabled: Up=32 Right=33 Down=25 Left=14 A=23 B=19");
    portENTER_CRITICAL(&s_snapshot_mux);
    s_snapshot.enabled = true;
    portEXIT_CRITICAL(&s_snapshot_mux);
#endif
    return ESP_OK;
}

#include "ir_camera_service.h"

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ir_camera.h"
#include "wiimote_web.h"

#if CONFIG_EXAMPLE_WIFI_WEB_CONTROL
#define IR_CAMERA_POLL_MS 50
static const char *TAG = "ir_camera_service";
#endif

#if CONFIG_EXAMPLE_WIFI_WEB_CONTROL
static void ir_camera_task(void *arg)
{
    (void)arg;
    ir_camera_state_t state = {0};
    esp_err_t last_error = ESP_OK;

    for (;;) {
        esp_err_t err = ir_camera_read(&state);
        if (err != ESP_OK && err != last_error) {
            ESP_LOGW(TAG, "IR camera frame read failed: %s", esp_err_to_name(err));
        } else if (err == ESP_OK && last_error != ESP_OK) {
            ESP_LOGI(TAG, "IR camera frame reads recovered");
        }
        last_error = err;
        wiimote_web_set_camera_state(&state);
        vTaskDelay(pdMS_TO_TICKS(IR_CAMERA_POLL_MS));
    }
}
#endif

void ir_camera_service_start(void)
{
#if CONFIG_EXAMPLE_WIFI_WEB_CONTROL
    ir_camera_state_t camera_state = {0};
    esp_err_t camera_err = ir_camera_init();
    if (camera_err == ESP_OK) {
        camera_state.detected = true;
        wiimote_web_set_camera_state(&camera_state);
        BaseType_t task_result = xTaskCreate(ir_camera_task, "ir_camera", 3072,
                                             NULL, 5, NULL);
        if (task_result != pdPASS) {
            ESP_LOGE(TAG, "Failed to create IR camera polling task");
        }
    } else {
        ESP_LOGW(TAG, "IR camera initialization at 0x58 failed: %s; continuing without camera",
                 esp_err_to_name(camera_err));
        wiimote_web_set_camera_state(&camera_state);
    }
#endif
}

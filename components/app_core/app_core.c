/*
 * app_core
 *
 * Responsibility: Owns the Mini-CW application state machine, initializes
 * services, and routes high-level events between services.
 * Hardware ownership: none. app_core only talks to service/HAL APIs and must
 * not directly access board hardware.
 */

#include "app_core.h"

#include "audio_service.h"
#include "cw_trainer_service.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "keyer_service.h"
#include "platform_hal.h"
#include "storage_service.h"
#include "ui_service.h"

#include <stdbool.h>
#include <stdint.h>

static const char *TAG = "app_core";

#define APP_INPUT_POLL_MS 5U

static TickType_t app_core_ms_to_delay_ticks(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return ticks > 0 ? ticks : 1;
}

typedef struct {
    app_mode_t mode;
    bool initialized;
} app_state_t;

static app_state_t s_app = {
    .mode = APP_MODE_KEYER,
    .initialized = false,
};

static void app_core_handle_ui_event(ui_input_event_t event)
{
    if (event.type == UI_INPUT_EVENT_NONE) {
        return;
    }

    if (event.type == UI_INPUT_EVENT_CANCEL) {
        ESP_LOGI(TAG, "cancel input received");
        audio_service_stop_all();
        return;
    }

    if (event.type == UI_INPUT_EVENT_SLEEP_REQUEST) {
        ESP_LOGI(TAG, "sleep input received");
        audio_service_stop_all();
        ui_service_prepare_for_sleep();
        vTaskDelay(pdMS_TO_TICKS(100));
        platform_hal_enter_deep_sleep();
        return;
    }
}

void app_core_init(void)
{
    ESP_LOGI(TAG, "Mini-CW service initialization starting");

    ESP_LOGI(TAG, "init: platform_hal");
    platform_hal_init();

    ESP_LOGI(TAG, "init: audio_service");
    audio_service_init();

    ESP_LOGI(TAG, "init: keyer_service");
    keyer_service_init();

    ESP_LOGI(TAG, "init: ui_service");
    ui_service_init();

    ESP_LOGI(TAG, "init: storage_service");
    storage_service_init();
    storage_profile_load();

    ESP_LOGI(TAG, "init: cw_trainer_service");
    cw_trainer_service_init();

    s_app.initialized = true;
    ui_service_show_demo_screen();

    ESP_LOGI(TAG, "Mini-CW service initialization complete");
}

void app_core_run(void)
{
    if (!s_app.initialized) {
        app_core_init();
    }

    ESP_LOGI(TAG, "Mini-CW app loop started");

    for (;;) {
        keyer_service_update();
        ui_input_event_t event = ui_service_poll_input();
        app_core_handle_ui_event(event);
        vTaskDelay(app_core_ms_to_delay_ticks(APP_INPUT_POLL_MS));
    }
}

app_mode_t app_core_get_mode(void)
{
    return s_app.mode;
}

const char *app_core_mode_to_string(app_mode_t mode)
{
    switch (mode) {
    case APP_MODE_PRACTICE:
        return "Practice";
    case APP_MODE_KEYER:
        return "Keyer";
    case APP_MODE_LESSONS:
        return "Lessons";
    default:
        return "Unknown";
    }
}

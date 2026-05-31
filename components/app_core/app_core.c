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

static app_mode_t app_core_ui_mode_to_app(ui_service_mode_t mode)
{
    switch (mode) {
    case UI_SERVICE_MODE_PRACTICE:
        return APP_MODE_PRACTICE;
    case UI_SERVICE_MODE_LESSONS:
        return APP_MODE_LESSONS;
    case UI_SERVICE_MODE_KEYER:
    default:
        return APP_MODE_KEYER;
    }
}

static void app_core_sync_mode_from_ui(void)
{
    s_app.mode = app_core_ui_mode_to_app(ui_service_get_mode());
    ESP_LOGI(TAG, "active mode: %s", app_core_mode_to_string(s_app.mode));
}

static void app_core_handle_lesson_select(void)
{
    const cw_lesson_view_t *view = cw_trainer_lesson_get_view();

    if (view != NULL && view->state == CW_LESSON_STATE_COPYING) {
        const cw_lesson_result_t *result = cw_trainer_lesson_submit();
        storage_lesson_save_result(result);
    } else {
        cw_trainer_lesson_start();
        storage_lesson_save_config(cw_trainer_lesson_get_config());
    }

    ui_service_refresh();
}

static void app_core_handle_char_input(char key)
{
    if (s_app.mode == APP_MODE_LESSONS) {
        cw_trainer_lesson_append_char(key);
    } else {
        cw_trainer_handle_char_input(key);
    }

    ui_service_refresh();
}

static void app_core_handle_ui_event(ui_input_event_t event)
{
    if (event.type == UI_INPUT_EVENT_NONE) {
        return;
    }

    if (event.type == UI_INPUT_EVENT_CANCEL) {
        ESP_LOGI(TAG, "cancel input received");
        if (s_app.mode == APP_MODE_LESSONS) {
            cw_trainer_lesson_abort();
        } else {
            audio_service_stop_all();
            cw_trainer_stop();
        }
        ui_service_refresh();
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

    switch (event.type) {
    case UI_INPUT_EVENT_MODE_CHANGED:
        app_core_sync_mode_from_ui();
        ui_service_refresh();
        break;
    case UI_INPUT_EVENT_LESSON_CONFIG_CHANGED:
        storage_lesson_save_config(cw_trainer_lesson_get_config());
        ui_service_refresh();
        break;
    case UI_INPUT_EVENT_SELECT:
        if (s_app.mode == APP_MODE_LESSONS) {
            app_core_handle_lesson_select();
        }
        break;
    case UI_INPUT_EVENT_CHAR_INPUT:
        app_core_handle_char_input(event.key);
        break;
    case UI_INPUT_EVENT_BACKSPACE:
        if (s_app.mode == APP_MODE_LESSONS) {
            cw_trainer_lesson_backspace();
            ui_service_refresh();
        }
        break;
    case UI_INPUT_EVENT_WPM_UP:
        cw_trainer_adjust_wpm(1);
        ui_service_refresh();
        break;
    case UI_INPUT_EVENT_WPM_DOWN:
        cw_trainer_adjust_wpm(-1);
        ui_service_refresh();
        break;
    case UI_INPUT_EVENT_PITCH_UP:
        cw_trainer_adjust_pitch(50);
        ui_service_refresh();
        break;
    case UI_INPUT_EVENT_PITCH_DOWN:
        cw_trainer_adjust_pitch(-50);
        ui_service_refresh();
        break;
    case UI_INPUT_EVENT_NONE:
    case UI_INPUT_EVENT_CANCEL:
    case UI_INPUT_EVENT_SLEEP_REQUEST:
    default:
        break;
    }
}

void app_core_init(void)
{
    ESP_LOGI(TAG, "Mini-CW service initialization starting");

    ESP_LOGI(TAG, "init: platform_hal");
    platform_hal_init();

    ESP_LOGI(TAG, "init: storage_service");
    storage_service_init();
    storage_profile_load();

    ESP_LOGI(TAG, "init: audio_service");
    audio_service_init();

    ESP_LOGI(TAG, "init: keyer_service");
    keyer_service_init();

    ESP_LOGI(TAG, "init: ui_service");
    ui_service_init();

    ESP_LOGI(TAG, "init: cw_trainer_service");
    cw_trainer_service_init();
    cw_lesson_config_t lesson_config = *cw_trainer_lesson_get_config();
    cw_lesson_result_t lesson_result = {0};
    if (storage_lesson_load(&lesson_config, &lesson_result)) {
        cw_trainer_lesson_load_persisted(&lesson_config, &lesson_result);
    }

    s_app.initialized = true;
    app_core_sync_mode_from_ui();
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

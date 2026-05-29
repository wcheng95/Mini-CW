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

static const char *TAG = "app_core";

#define APP_INPUT_POLL_MS 20U
#define APP_TONE_STEP_HZ 50
#define APP_VOLUME_STEP 5

typedef struct {
    app_mode_t mode;
    bool initialized;
    bool bottom_edit_mode;
} app_state_t;

static app_state_t s_app = {
    .mode = APP_MODE_TONE_TEST,
    .initialized = false,
    .bottom_edit_mode = false,
};

static void app_core_redraw_current_screen(void)
{
    ui_service_show_demo_screen();
}

static void app_core_set_bottom_edit_mode(bool active)
{
    s_app.bottom_edit_mode = active;
    ui_service_set_bottom_edit_mode(active);
    ESP_LOGI(TAG, "bottom edit mode: %s", active ? "on" : "off");
    app_core_redraw_current_screen();
}

static void app_core_handle_bottom_edit_key(char key)
{
    bool handled = true;

    switch (key) {
    case '1':
        keyer_service_adjust_tx_wpm(-1);
        break;
    case '2':
        keyer_service_adjust_tx_wpm(1);
        break;
    case '3':
        audio_service_adjust_tone_hz(-APP_TONE_STEP_HZ);
        break;
    case '4':
        audio_service_adjust_tone_hz(APP_TONE_STEP_HZ);
        break;
    case '5':
    {
        uint8_t previous_volume = audio_service_get_volume();
        audio_service_adjust_volume(-APP_VOLUME_STEP);
        if (audio_service_get_volume() != previous_volume) {
            audio_service_play_feedback_tone();
        }
        break;
    }
    case '6':
    {
        uint8_t previous_volume = audio_service_get_volume();
        audio_service_adjust_volume(APP_VOLUME_STEP);
        if (audio_service_get_volume() != previous_volume) {
            audio_service_play_feedback_tone();
        }
        break;
    }
    default:
        handled = false;
        break;
    }

    if (handled) {
        app_core_redraw_current_screen();
    }
}

static void app_core_handle_ui_event(ui_input_event_t event)
{
    if (event.type == UI_INPUT_EVENT_NONE) {
        return;
    }

    if (event.type == UI_INPUT_EVENT_FN) {
        app_core_set_bottom_edit_mode(!s_app.bottom_edit_mode);
        return;
    }

    if (event.type == UI_INPUT_EVENT_CANCEL) {
        if (s_app.bottom_edit_mode) {
            app_core_set_bottom_edit_mode(false);
            return;
        }

        ESP_LOGI(TAG, "cancel input received");
        audio_cw_stop();
        return;
    }

    if (!s_app.bottom_edit_mode) {
        return;
    }

    if (event.type == UI_INPUT_EVENT_CHAR_INPUT) {
        app_core_handle_bottom_edit_key(event.key);
    }
}

void app_core_init(void)
{
    ESP_LOGI(TAG, "Mini-CW service initialization starting");

    ESP_LOGI(TAG, "init: platform_hal");
    platform_hal_init();

    ESP_LOGI(TAG, "init: ui_service");
    ui_service_init();

    ESP_LOGI(TAG, "init: audio_service");
    audio_service_init();

    ESP_LOGI(TAG, "init: keyer_service");
    keyer_service_init();

    ESP_LOGI(TAG, "init: storage_service");
    storage_service_init();
    storage_profile_load();

    ESP_LOGI(TAG, "init: cw_trainer_service");
    cw_trainer_service_init();

    s_app.initialized = true;
    ui_service_set_bottom_edit_mode(s_app.bottom_edit_mode);
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
        ui_input_event_t event = ui_service_poll_input();
        app_core_handle_ui_event(event);
        vTaskDelay(pdMS_TO_TICKS(APP_INPUT_POLL_MS));
    }
}

app_mode_t app_core_get_mode(void)
{
    return s_app.mode;
}

const char *app_core_mode_to_string(app_mode_t mode)
{
    switch (mode) {
    case APP_MODE_TONE_TEST:
        return "Tone Test";
    case APP_MODE_RX_PRACTICE:
        return "RX Practice";
    case APP_MODE_TX_PRACTICE:
        return "TX Practice";
    case APP_MODE_CALLSIGN:
        return "Callsign";
    case APP_MODE_QSO:
        return "QSO";
    case APP_MODE_STATS:
        return "Stats";
    case APP_MODE_MENU:
        return "Menu";
    default:
        return "Unknown";
    }
}

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

typedef struct {
    app_mode_t mode;
    bool initialized;
} app_state_t;

static app_state_t s_app = {
    .mode = APP_MODE_TONE_TEST,
    .initialized = false,
};

static void app_core_show_tone_test(void)
{
    ui_tone_test_view_t view = {
        .mode_name = app_core_mode_to_string(s_app.mode),
        .last_char = cw_trainer_get_last_char(),
        .last_pattern = cw_trainer_get_last_pattern(),
        .wpm = audio_cw_get_wpm(),
        .pitch_hz = audio_cw_get_pitch(),
        .status = cw_trainer_get_status(),
    };

    ui_service_show_tone_test(&view);
}

static void app_core_set_mode(app_mode_t mode)
{
    if (s_app.mode == mode) {
        return;
    }

    ESP_LOGI(TAG, "mode change: %s -> %s",
             app_core_mode_to_string(s_app.mode),
             app_core_mode_to_string(mode));

    cw_trainer_stop();
    s_app.mode = mode;

    switch (mode) {
    case APP_MODE_TONE_TEST:
        cw_trainer_start_tone_test();
        break;
    case APP_MODE_RX_PRACTICE:
        cw_trainer_start_rx_practice();
        break;
    case APP_MODE_TX_PRACTICE:
        cw_trainer_start_tx_practice();
        break;
    case APP_MODE_CALLSIGN:
    case APP_MODE_QSO:
    case APP_MODE_STATS:
    case APP_MODE_MENU:
    default:
        break;
    }

    if (mode == APP_MODE_TONE_TEST) {
        app_core_show_tone_test();
    } else {
        ui_service_show_home(app_core_mode_to_string(s_app.mode));
    }
}

static void app_core_handle_ui_event(ui_input_event_t event)
{
    switch (event.type) {
    case UI_INPUT_EVENT_CHAR_INPUT:
        cw_trainer_handle_char_input(event.key);
        app_core_show_tone_test();
        break;
    case UI_INPUT_EVENT_WPM_UP:
        cw_trainer_adjust_wpm(1);
        app_core_show_tone_test();
        break;
    case UI_INPUT_EVENT_WPM_DOWN:
        cw_trainer_adjust_wpm(-1);
        app_core_show_tone_test();
        break;
    case UI_INPUT_EVENT_PITCH_UP:
        cw_trainer_adjust_pitch(50);
        app_core_show_tone_test();
        break;
    case UI_INPUT_EVENT_PITCH_DOWN:
        cw_trainer_adjust_pitch(-50);
        app_core_show_tone_test();
        break;
    case UI_INPUT_EVENT_MODE_RX:
        app_core_set_mode(APP_MODE_RX_PRACTICE);
        break;
    case UI_INPUT_EVENT_MODE_TX:
        app_core_set_mode(APP_MODE_TX_PRACTICE);
        break;
    case UI_INPUT_EVENT_MODE_CALLSIGN:
        app_core_set_mode(APP_MODE_CALLSIGN);
        break;
    case UI_INPUT_EVENT_MODE_QSO:
        app_core_set_mode(APP_MODE_QSO);
        break;
    case UI_INPUT_EVENT_MODE_STATS:
        app_core_set_mode(APP_MODE_STATS);
        break;
    case UI_INPUT_EVENT_MODE_MENU:
        app_core_set_mode(APP_MODE_MENU);
        break;
    case UI_INPUT_EVENT_CANCEL:
        ESP_LOGI(TAG, "cancel input received");
        audio_cw_stop();
        cw_trainer_stop();
        app_core_show_tone_test();
        break;
    case UI_INPUT_EVENT_SELECT:
        ESP_LOGI(TAG, "select input received");
        break;
    case UI_INPUT_EVENT_NONE:
    default:
        break;
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
        vTaskDelay(pdMS_TO_TICKS(1000));
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

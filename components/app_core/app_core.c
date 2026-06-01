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
    case UI_SERVICE_MODE_PLAINTEXT:
        return APP_MODE_PLAINTEXT;
    case UI_SERVICE_MODE_LESSONS:
        return APP_MODE_LESSONS;
    case UI_SERVICE_MODE_WORDS:
        return APP_MODE_WORDS;
    case UI_SERVICE_MODE_CALLSIGNS:
        return APP_MODE_CALLSIGNS;
    case UI_SERVICE_MODE_SYSTEM:
        return APP_MODE_SYSTEM;
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

static void app_core_handle_word_select(void)
{
    const cw_word_view_t *view = cw_trainer_word_get_view();

    if (view != NULL && view->state == CW_WORD_STATE_COPYING) {
        const cw_word_result_t *result = cw_trainer_word_submit();
        const cw_word_view_t *updated_view = cw_trainer_word_get_view();
        if (updated_view != NULL && updated_view->state == CW_WORD_STATE_RESULT) {
            storage_word_save_result(result);
        }
    } else {
        cw_trainer_word_start();
        storage_word_save_config(cw_trainer_word_get_config());
    }

    ui_service_refresh();
}

static void app_core_handle_callsign_select(void)
{
    const cw_callsign_view_t *view = cw_trainer_callsign_get_view();

    if (view != NULL && view->state == CW_CALLSIGN_STATE_COPYING) {
        const cw_callsign_result_t *result = cw_trainer_callsign_submit();
        const cw_callsign_view_t *updated_view = cw_trainer_callsign_get_view();
        if (updated_view != NULL && updated_view->state == CW_CALLSIGN_STATE_RESULT) {
            storage_callsign_save_result(result);
        }
    } else {
        cw_trainer_callsign_start();
        storage_callsign_save_config(cw_trainer_callsign_get_config());
    }

    ui_service_refresh();
}

static void app_core_handle_plaintext_select(void)
{
    const cw_plaintext_view_t *view = cw_trainer_plaintext_get_view();

    if (view != NULL && view->state == CW_PLAINTEXT_STATE_COPYING) {
        const cw_plaintext_result_t *result = cw_trainer_plaintext_submit();
        storage_plaintext_save_result(result);
    } else {
        cw_trainer_plaintext_start();
        storage_plaintext_save_config(cw_trainer_plaintext_get_config());
    }

    ui_service_refresh();
}

static void app_core_handle_volume_changed(const ui_input_event_t *event)
{
    if (event == NULL) {
        return;
    }

    audio_service_set_volume((uint8_t)event->value);
    audio_service_play_feedback_beep();
    ui_service_refresh();
}

static void app_core_handle_key_in_wpm_changed(const ui_input_event_t *event)
{
    if (event == NULL) {
        return;
    }

    keyer_service_set_key_in_wpm((uint8_t)event->value);
    ui_service_refresh();
}

static void app_core_handle_key_in_mode_changed(const ui_input_event_t *event)
{
    int direction = 1;

    if (event != NULL && event->delta != 0) {
        direction = event->delta;
    }

    keyer_service_cycle_key_in_mode(direction);
    ui_service_refresh();
}

static void app_core_handle_lesson_config_changed(const ui_input_event_t *event)
{
    cw_lesson_config_t config = *cw_trainer_lesson_get_config();

    if (event != NULL) {
        switch (event->setting) {
        case UI_SETTING_LESSON:
            config.lesson = (uint8_t)event->value;
            break;
        case UI_SETTING_LESSON_DURATION:
            config.duration_min = (uint8_t)event->value;
            break;
        case UI_SETTING_LESSON_CODE_WPM:
            config.code_wpm = (uint8_t)event->value;
            break;
        case UI_SETTING_LESSON_EFFECTIVE_WPM:
            config.effective_wpm = (uint8_t)event->value;
            break;
        case UI_SETTING_LESSON_GROUP_LEN:
            config.group_len = (uint8_t)event->value;
            break;
        case UI_SETTING_NONE:
        case UI_SETTING_VOLUME:
        case UI_SETTING_KEY_IN_WPM:
        case UI_SETTING_KEY_IN_MODE:
        case UI_SETTING_WORD_SPEED:
        case UI_SETTING_WORD_MIN_CHAR_WPM:
        case UI_SETTING_WORD_LESSON:
        case UI_SETTING_WORD_MAX_LEN:
        case UI_SETTING_CALLSIGN_SPEED:
        case UI_SETTING_CALLSIGN_MIN_CHAR_WPM:
        case UI_SETTING_CALLSIGN_MAX_WPM:
        case UI_SETTING_PLAINTEXT_CODE_WPM:
        case UI_SETTING_PLAINTEXT_EFFECTIVE_WPM:
        default:
            break;
        }
    }

    /* UI reports intent; app_core applies trainer state and persistence routing. */
    cw_trainer_lesson_set_config(&config);
    storage_lesson_save_config(cw_trainer_lesson_get_config());
    ui_service_refresh();
}

static void app_core_handle_word_config_changed(const ui_input_event_t *event)
{
    cw_word_config_t config = *cw_trainer_word_get_config();

    if (event != NULL) {
        switch (event->setting) {
        case UI_SETTING_WORD_SPEED:
            config.start_wpm = (uint8_t)event->value;
            break;
        case UI_SETTING_WORD_MIN_CHAR_WPM:
            config.min_char_wpm = (uint8_t)event->value;
            break;
        case UI_SETTING_WORD_LESSON:
            config.lesson = (uint8_t)event->value;
            break;
        case UI_SETTING_WORD_MAX_LEN:
            config.max_word_len = (uint8_t)event->value;
            break;
        case UI_SETTING_NONE:
        case UI_SETTING_VOLUME:
        case UI_SETTING_KEY_IN_WPM:
        case UI_SETTING_KEY_IN_MODE:
        case UI_SETTING_LESSON:
        case UI_SETTING_LESSON_DURATION:
        case UI_SETTING_LESSON_CODE_WPM:
        case UI_SETTING_LESSON_EFFECTIVE_WPM:
        case UI_SETTING_LESSON_GROUP_LEN:
        case UI_SETTING_CALLSIGN_SPEED:
        case UI_SETTING_CALLSIGN_MIN_CHAR_WPM:
        case UI_SETTING_CALLSIGN_MAX_WPM:
        case UI_SETTING_PLAINTEXT_CODE_WPM:
        case UI_SETTING_PLAINTEXT_EFFECTIVE_WPM:
        default:
            break;
        }
    }

    /* UI reports intent; app_core applies trainer state and persistence routing. */
    cw_trainer_word_set_config(&config);
    storage_word_save_config(cw_trainer_word_get_config());
    ui_service_refresh();
}

static void app_core_handle_callsign_config_changed(const ui_input_event_t *event)
{
    cw_callsign_config_t config = *cw_trainer_callsign_get_config();

    if (event != NULL) {
        switch (event->setting) {
        case UI_SETTING_CALLSIGN_SPEED:
            config.start_wpm = (uint8_t)event->value;
            break;
        case UI_SETTING_CALLSIGN_MIN_CHAR_WPM:
            config.min_char_wpm = (uint8_t)event->value;
            break;
        case UI_SETTING_CALLSIGN_MAX_WPM:
            config.max_wpm = (uint8_t)event->value;
            break;
        case UI_SETTING_NONE:
        case UI_SETTING_VOLUME:
        case UI_SETTING_KEY_IN_WPM:
        case UI_SETTING_KEY_IN_MODE:
        case UI_SETTING_LESSON:
        case UI_SETTING_LESSON_DURATION:
        case UI_SETTING_LESSON_CODE_WPM:
        case UI_SETTING_LESSON_EFFECTIVE_WPM:
        case UI_SETTING_LESSON_GROUP_LEN:
        case UI_SETTING_WORD_SPEED:
        case UI_SETTING_WORD_MIN_CHAR_WPM:
        case UI_SETTING_WORD_LESSON:
        case UI_SETTING_WORD_MAX_LEN:
        case UI_SETTING_PLAINTEXT_CODE_WPM:
        case UI_SETTING_PLAINTEXT_EFFECTIVE_WPM:
        default:
            break;
        }
    }

    /* UI reports intent; app_core applies trainer state and persistence routing. */
    cw_trainer_callsign_set_config(&config);
    storage_callsign_save_config(cw_trainer_callsign_get_config());
    ui_service_refresh();
}

static void app_core_handle_plaintext_config_changed(const ui_input_event_t *event)
{
    cw_plaintext_config_t config = *cw_trainer_plaintext_get_config();

    if (event != NULL) {
        switch (event->setting) {
        case UI_SETTING_PLAINTEXT_CODE_WPM:
            config.code_wpm = (uint8_t)event->value;
            break;
        case UI_SETTING_PLAINTEXT_EFFECTIVE_WPM:
            config.effective_wpm = (uint8_t)event->value;
            break;
        case UI_SETTING_NONE:
        case UI_SETTING_VOLUME:
        case UI_SETTING_KEY_IN_WPM:
        case UI_SETTING_KEY_IN_MODE:
        case UI_SETTING_LESSON:
        case UI_SETTING_LESSON_DURATION:
        case UI_SETTING_LESSON_CODE_WPM:
        case UI_SETTING_LESSON_EFFECTIVE_WPM:
        case UI_SETTING_LESSON_GROUP_LEN:
        case UI_SETTING_WORD_SPEED:
        case UI_SETTING_WORD_MIN_CHAR_WPM:
        case UI_SETTING_WORD_LESSON:
        case UI_SETTING_WORD_MAX_LEN:
        case UI_SETTING_CALLSIGN_SPEED:
        case UI_SETTING_CALLSIGN_MIN_CHAR_WPM:
        case UI_SETTING_CALLSIGN_MAX_WPM:
        default:
            break;
        }
    }

    /* UI reports intent; app_core applies trainer state and persistence routing. */
    cw_trainer_plaintext_set_config(&config);
    storage_plaintext_save_config(cw_trainer_plaintext_get_config());
    ui_service_refresh();
}

static void app_core_handle_char_input(char key)
{
    if (s_app.mode == APP_MODE_PLAINTEXT) {
        cw_trainer_plaintext_append_char(key);
    } else if (s_app.mode == APP_MODE_LESSONS) {
        cw_trainer_lesson_append_char(key);
    } else if (s_app.mode == APP_MODE_WORDS) {
        cw_trainer_word_append_char(key);
    } else if (s_app.mode == APP_MODE_CALLSIGNS) {
        cw_trainer_callsign_append_char(key);
    } else if (s_app.mode == APP_MODE_KEYER) {
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
        } else if (s_app.mode == APP_MODE_PLAINTEXT) {
            cw_trainer_plaintext_abort();
        } else if (s_app.mode == APP_MODE_WORDS) {
            cw_trainer_word_abort();
        } else if (s_app.mode == APP_MODE_CALLSIGNS) {
            cw_trainer_callsign_abort();
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
    case UI_INPUT_EVENT_VOLUME_CHANGED:
        app_core_handle_volume_changed(&event);
        break;
    case UI_INPUT_EVENT_KEY_IN_WPM_CHANGED:
        app_core_handle_key_in_wpm_changed(&event);
        break;
    case UI_INPUT_EVENT_KEY_IN_MODE_CHANGED:
        app_core_handle_key_in_mode_changed(&event);
        break;
    case UI_INPUT_EVENT_LESSON_CONFIG_CHANGED:
        app_core_handle_lesson_config_changed(&event);
        break;
    case UI_INPUT_EVENT_WORD_CONFIG_CHANGED:
        app_core_handle_word_config_changed(&event);
        break;
    case UI_INPUT_EVENT_CALLSIGN_CONFIG_CHANGED:
        app_core_handle_callsign_config_changed(&event);
        break;
    case UI_INPUT_EVENT_PLAINTEXT_CONFIG_CHANGED:
        app_core_handle_plaintext_config_changed(&event);
        break;
    case UI_INPUT_EVENT_SELECT:
        if (s_app.mode == APP_MODE_PLAINTEXT) {
            app_core_handle_plaintext_select();
        } else if (s_app.mode == APP_MODE_LESSONS) {
            app_core_handle_lesson_select();
        } else if (s_app.mode == APP_MODE_WORDS) {
            app_core_handle_word_select();
        } else if (s_app.mode == APP_MODE_CALLSIGNS) {
            app_core_handle_callsign_select();
        }
        break;
    case UI_INPUT_EVENT_CHAR_INPUT:
        app_core_handle_char_input(event.key);
        break;
    case UI_INPUT_EVENT_BACKSPACE:
        if (s_app.mode == APP_MODE_PLAINTEXT) {
            cw_trainer_plaintext_backspace();
            ui_service_refresh();
        } else if (s_app.mode == APP_MODE_LESSONS) {
            cw_trainer_lesson_backspace();
            ui_service_refresh();
        } else if (s_app.mode == APP_MODE_WORDS) {
            cw_trainer_word_backspace();
            ui_service_refresh();
        } else if (s_app.mode == APP_MODE_CALLSIGNS) {
            cw_trainer_callsign_backspace();
            ui_service_refresh();
        }
        break;
    case UI_INPUT_EVENT_REPLAY:
        if (s_app.mode == APP_MODE_WORDS) {
            cw_trainer_word_replay();
            ui_service_refresh();
        } else if (s_app.mode == APP_MODE_CALLSIGNS) {
            cw_trainer_callsign_replay();
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
    cw_word_config_t word_config = *cw_trainer_word_get_config();
    cw_word_result_t word_result = {0};
    if (storage_word_load(&word_config, &word_result)) {
        cw_trainer_word_load_persisted(&word_config, &word_result);
    }
    cw_callsign_config_t callsign_config = *cw_trainer_callsign_get_config();
    cw_callsign_result_t callsign_result = {0};
    if (storage_callsign_load(&callsign_config, &callsign_result)) {
        cw_trainer_callsign_load_persisted(&callsign_config, &callsign_result);
    }
    cw_plaintext_config_t plaintext_config = *cw_trainer_plaintext_get_config();
    cw_plaintext_result_t plaintext_result = {0};
    if (storage_plaintext_load(&plaintext_config, &plaintext_result)) {
        cw_trainer_plaintext_load_persisted(&plaintext_config, &plaintext_result);
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
    case APP_MODE_PLAINTEXT:
        return "Plain";
    case APP_MODE_KEYER:
        return "Keyer";
    case APP_MODE_LESSONS:
        return "Lessons";
    case APP_MODE_WORDS:
        return "Words";
    case APP_MODE_CALLSIGNS:
        return "Calls";
    case APP_MODE_SYSTEM:
        return "System";
    default:
        return "Unknown";
    }
}

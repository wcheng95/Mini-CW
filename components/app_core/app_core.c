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

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "app_core";

#define APP_INPUT_POLL_MS 5U
#define APP_SYSTEM_CONFIG_SAVE_DELAY_MS 1000U
#define APP_KEYER_TX_BUFFER_MAX 127U
#define APP_KEYER_MACRO_TEXT_MAX 255U

static TickType_t app_core_ms_to_delay_ticks(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return ticks > 0 ? ticks : 1;
}

static bool app_core_tick_reached(TickType_t now, TickType_t due)
{
    return (TickType_t)(now - due) < (TickType_t)(UINT32_MAX / 2U);
}

typedef struct {
    app_mode_t mode;
    bool initialized;
} app_state_t;

static app_state_t s_app = {
    .mode = APP_MODE_KEYER,
    .initialized = false,
};
static bool s_system_config_dirty;
static TickType_t s_system_config_save_due;
static bool s_keyer_config_dirty;
static TickType_t s_keyer_config_save_due;

typedef struct {
    char tx_buffer[APP_KEYER_TX_BUFFER_MAX + 1U];
    uint8_t tx_len;
    bool tx_pending;
    TickType_t tx_due;

    bool macro_active;
    uint8_t macro_index;
    bool macro_pending_start;
    TickType_t macro_start_due;
    bool macro_repeat_enabled;
    bool macro_waiting_repeat;
    TickType_t macro_repeat_due;
    char macro_text[APP_KEYER_MACRO_TEXT_MAX + 1U];
    uint16_t macro_pos;
} app_keyer_state_t;

static app_keyer_state_t s_keyer;

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

static storage_system_config_t app_core_current_system_config(void)
{
    return (storage_system_config_t){
        .volume = audio_service_get_volume(),
        .tone_hz = audio_service_get_tone_hz(),
        .key_in_mode = keyer_service_get_key_in_mode(),
        .key_in_wpm = keyer_service_get_key_in_wpm(),
    };
}

static bool app_core_system_config_equal(const storage_system_config_t *a,
                                         const storage_system_config_t *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }

    return a->volume == b->volume && a->tone_hz == b->tone_hz &&
           a->key_in_mode == b->key_in_mode && a->key_in_wpm == b->key_in_wpm;
}

static void app_core_mark_system_config_dirty(void)
{
    s_system_config_dirty = true;
    s_system_config_save_due =
        xTaskGetTickCount() + app_core_ms_to_delay_ticks(APP_SYSTEM_CONFIG_SAVE_DELAY_MS);
}

static void app_core_mark_keyer_config_dirty(void)
{
    s_keyer_config_dirty = true;
    s_keyer_config_save_due =
        xTaskGetTickCount() + app_core_ms_to_delay_ticks(APP_SYSTEM_CONFIG_SAVE_DELAY_MS);
}

static void app_core_maybe_save_dirty_config(void)
{
    TickType_t now;
    storage_system_config_t config;
    keyer_config_t keyer_config;

    if (!s_system_config_dirty && !s_keyer_config_dirty) {
        return;
    }

    now = xTaskGetTickCount();

    if (audio_service_is_busy() || keyer_service_is_tx_active()) {
        if (s_system_config_dirty) {
            s_system_config_save_due =
                now + app_core_ms_to_delay_ticks(APP_SYSTEM_CONFIG_SAVE_DELAY_MS);
        }
        if (s_keyer_config_dirty) {
            s_keyer_config_save_due =
                now + app_core_ms_to_delay_ticks(APP_SYSTEM_CONFIG_SAVE_DELAY_MS);
        }
        return;
    }

    if (s_system_config_dirty && app_core_tick_reached(now, s_system_config_save_due)) {
        config = app_core_current_system_config();
        if (storage_system_save_config(&config)) {
            s_system_config_dirty = false;
        } else {
            s_system_config_save_due =
                now + app_core_ms_to_delay_ticks(APP_SYSTEM_CONFIG_SAVE_DELAY_MS);
        }
    }

    if (s_keyer_config_dirty && app_core_tick_reached(now, s_keyer_config_save_due)) {
        keyer_service_get_config_copy(&keyer_config);
        if (storage_keyer_save_config(&keyer_config)) {
            s_keyer_config_dirty = false;
        } else {
            s_keyer_config_save_due =
                now + app_core_ms_to_delay_ticks(APP_SYSTEM_CONFIG_SAVE_DELAY_MS);
        }
    }
}

static void app_core_apply_system_config(const storage_system_config_t *config)
{
    if (config == NULL) {
        return;
    }

    audio_service_set_volume(config->volume);
    audio_service_set_tone_hz(config->tone_hz);
    keyer_service_set_key_in_mode(config->key_in_mode);
    keyer_service_set_key_in_wpm(config->key_in_wpm);
}

static void app_core_load_persisted_settings(void)
{
    storage_system_config_t system_config;
    keyer_config_t keyer_config;
    cw_lesson_config_t lesson_config;
    cw_lesson_result_t lesson_result = {0};
    cw_word_config_t word_config;
    cw_word_result_t word_result = {0};
    cw_callsign_config_t callsign_config;
    cw_callsign_result_t callsign_result = {0};
    cw_plaintext_config_t plaintext_config;
    cw_plaintext_result_t plaintext_result = {0};

    if (!storage_profile_load()) {
        return;
    }

    system_config = app_core_current_system_config();
    if (storage_system_load_config(&system_config)) {
        app_core_apply_system_config(&system_config);
        storage_system_config_t applied_config = app_core_current_system_config();
        if (!app_core_system_config_equal(&system_config, &applied_config)) {
            storage_system_save_config(&applied_config);
        }
    }

    keyer_config = *keyer_service_get_config();
    if (storage_keyer_load_config(&keyer_config)) {
        keyer_service_set_config(&keyer_config);
    }

    lesson_config = *cw_trainer_lesson_get_config();
    if (storage_lesson_load(&lesson_config, &lesson_result)) {
        cw_trainer_lesson_load_persisted(&lesson_config, &lesson_result);
    }

    word_config = *cw_trainer_word_get_config();
    if (storage_word_load(&word_config, &word_result)) {
        cw_trainer_word_load_persisted(&word_config, &word_result);
    }

    callsign_config = *cw_trainer_callsign_get_config();
    if (storage_callsign_load(&callsign_config, &callsign_result)) {
        cw_trainer_callsign_load_persisted(&callsign_config, &callsign_result);
    }

    plaintext_config = *cw_trainer_plaintext_get_config();
    if (storage_plaintext_load(&plaintext_config, &plaintext_result)) {
        cw_trainer_plaintext_load_persisted(&plaintext_config, &plaintext_result);
    }
}

static void app_core_handle_lesson_select(void)
{
    const cw_lesson_view_t *view = cw_trainer_lesson_get_view();

    if (view != NULL && view->state == CW_LESSON_STATE_COPYING) {
        const cw_lesson_result_t *result = cw_trainer_lesson_submit();
        storage_lesson_save_result(result);
    } else {
        cw_trainer_lesson_start();
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
    }

    ui_service_refresh();
}

static void app_core_keyer_set_tx_display(void)
{
    ui_service_keyer_set_tx_text(s_keyer.tx_buffer);
}

static void app_core_keyer_clear_tx_buffer(void)
{
    s_keyer.tx_buffer[0] = '\0';
    s_keyer.tx_len = 0U;
    s_keyer.tx_pending = false;
    ui_service_keyer_clear_tx_text();
}

static void app_core_keyer_schedule_tx(void)
{
    s_keyer.tx_pending = true;
    s_keyer.tx_due =
        xTaskGetTickCount() +
        app_core_ms_to_delay_ticks((uint32_t)keyer_service_get_tx_delay_s() * 1000U);
}

static void app_core_keyer_send_buffer_now(void)
{
    if (s_keyer.tx_len == 0U) {
        return;
    }

    keyer_service_play_text(s_keyer.tx_buffer);
    app_core_keyer_clear_tx_buffer();
}

static void app_core_keyer_append_tx_char(char key)
{
    char normalized = key == ' ' ? ' ' : (char)toupper((unsigned char)key);

    if (normalized != ' ' && audio_service_get_cw_pattern(normalized) == NULL) {
        ui_service_keyer_set_status("Unsupported");
        return;
    }

    if (s_keyer.tx_len >= APP_KEYER_TX_BUFFER_MAX) {
        ui_service_keyer_set_status("TX buffer full");
        return;
    }

    s_keyer.tx_buffer[s_keyer.tx_len++] = normalized;
    s_keyer.tx_buffer[s_keyer.tx_len] = '\0';
    app_core_keyer_set_tx_display();
    app_core_keyer_schedule_tx();
}

static void app_core_keyer_backspace_tx(void)
{
    if (s_keyer.tx_len == 0U) {
        return;
    }

    --s_keyer.tx_len;
    s_keyer.tx_buffer[s_keyer.tx_len] = '\0';
    if (s_keyer.tx_len == 0U) {
        s_keyer.tx_pending = false;
    }
    app_core_keyer_set_tx_display();
}

static void app_core_keyer_cancel_macro(void)
{
    s_keyer.macro_active = false;
    s_keyer.macro_waiting_repeat = false;
    s_keyer.macro_pending_start = false;
    s_keyer.macro_index = 0U;
    s_keyer.macro_pos = 0U;
    keyer_service_stop_tx();
}

static void app_core_keyer_macro_continue(void)
{
    char segment[APP_KEYER_TX_BUFFER_MAX + 1U];
    uint16_t segment_len = 0U;

    if (!s_keyer.macro_active || s_keyer.macro_pending_start || keyer_service_is_tx_active()) {
        return;
    }

    if (s_keyer.macro_waiting_repeat) {
        if (!app_core_tick_reached(xTaskGetTickCount(), s_keyer.macro_repeat_due)) {
            return;
        }
        s_keyer.macro_waiting_repeat = false;
        s_keyer.macro_pos = 0U;
    }

    while (s_keyer.macro_text[s_keyer.macro_pos] != '\0' &&
           segment_len + 1U < sizeof(segment)) {
        segment[segment_len++] = s_keyer.macro_text[s_keyer.macro_pos++];
    }

    if (segment_len > 0U) {
        segment[segment_len] = '\0';
        keyer_service_play_text(segment);
        return;
    }

    if (s_keyer.macro_repeat_enabled) {
        s_keyer.macro_waiting_repeat = true;
        s_keyer.macro_repeat_due =
            xTaskGetTickCount() +
            app_core_ms_to_delay_ticks((uint32_t)keyer_service_get_repeat_interval_s() * 1000U);
        return;
    }

    s_keyer.macro_active = false;
    s_keyer.macro_index = 0U;
}

static void app_core_keyer_start_macro(uint8_t macro_index)
{
    if (macro_index < 1U || macro_index > KEYER_MESSAGE_COUNT) {
        return;
    }

    app_core_keyer_clear_tx_buffer();
    app_core_keyer_cancel_macro();
    s_keyer.macro_active = true;
    s_keyer.macro_index = macro_index;
    s_keyer.macro_pending_start = true;
    s_keyer.macro_start_due =
        xTaskGetTickCount() +
        app_core_ms_to_delay_ticks((uint32_t)keyer_service_get_tx_delay_s() * 1000U);
    s_keyer.macro_repeat_enabled = macro_index == 1U;
    s_keyer.macro_waiting_repeat = false;
    s_keyer.macro_pos = 0U;
    snprintf(s_keyer.macro_text,
             sizeof(s_keyer.macro_text),
             "%s",
             keyer_service_get_message((uint8_t)(macro_index - 1U)));
    ui_service_keyer_set_tx_text(s_keyer.macro_text);
}

static void app_core_keyer_start_pending_macro_now(void)
{
    if (!s_keyer.macro_active || !s_keyer.macro_pending_start) {
        return;
    }

    s_keyer.macro_pending_start = false;
    app_core_keyer_macro_continue();
}

static void app_core_keyer_update(void)
{
    if (s_keyer.macro_pending_start &&
        app_core_tick_reached(xTaskGetTickCount(), s_keyer.macro_start_due)) {
        app_core_keyer_start_pending_macro_now();
    }

    if (s_keyer.tx_pending && app_core_tick_reached(xTaskGetTickCount(), s_keyer.tx_due)) {
        app_core_keyer_send_buffer_now();
    }

    app_core_keyer_macro_continue();
}

static void app_core_handle_volume_changed(const ui_input_event_t *event)
{
    if (event == NULL) {
        return;
    }

    audio_service_set_volume((uint8_t)event->value);
    audio_service_play_feedback_beep();
    app_core_mark_system_config_dirty();
    ui_service_refresh();
}

static void app_core_handle_tone_changed(const ui_input_event_t *event)
{
    if (event == NULL) {
        return;
    }

    audio_service_set_tone_hz((uint16_t)event->value);
    audio_service_play_feedback_beep();
    app_core_mark_system_config_dirty();
    ui_service_refresh();
}

static void app_core_handle_key_in_wpm_changed(const ui_input_event_t *event)
{
    if (event == NULL) {
        return;
    }

    keyer_service_set_key_in_wpm((uint8_t)event->value);
    app_core_mark_system_config_dirty();
    ui_service_refresh();
}

static void app_core_handle_key_in_mode_changed(const ui_input_event_t *event)
{
    int direction = 1;

    if (event != NULL && event->delta != 0) {
        direction = event->delta;
    }

    keyer_service_cycle_key_in_mode(direction);
    app_core_mark_system_config_dirty();
    ui_service_refresh();
}

static void app_core_handle_key_out_mode_changed(const ui_input_event_t *event)
{
    int direction = 1;

    if (event != NULL && event->delta != 0) {
        direction = event->delta;
    }

    keyer_service_cycle_key_out_mode(direction);
    app_core_mark_keyer_config_dirty();
    ui_service_refresh();
}

static void app_core_handle_keyer_paddle_mode_changed(const ui_input_event_t *event)
{
    int direction = 1;

    if (event != NULL && event->delta != 0) {
        direction = event->delta;
    }

    keyer_service_cycle_paddle_mode(direction);
    app_core_mark_keyer_config_dirty();
    ui_service_refresh();
}

static void app_core_handle_keyer_mute_changed(const ui_input_event_t *event)
{
    if (event != NULL && event->setting == UI_SETTING_KEYER_MUTE) {
        keyer_service_set_mute(event->value != 0);
    } else {
        keyer_service_toggle_mute();
    }
    ui_service_refresh();
}

static void app_core_handle_keyer_config_changed(const ui_input_event_t *event)
{
    keyer_config_t config;

    if (event == NULL) {
        return;
    }

    keyer_service_get_config_copy(&config);
    switch (event->setting) {
    case UI_SETTING_KEYER_TX_DELAY_S:
        config.tx_delay_s = (uint8_t)event->value;
        break;
    case UI_SETTING_KEYER_REPEAT_INTERVAL_S:
        config.repeat_interval_s = (uint8_t)event->value;
        break;
    case UI_SETTING_KEYER_MESSAGE_1:
    case UI_SETTING_KEYER_MESSAGE_2:
    case UI_SETTING_KEYER_MESSAGE_3:
    case UI_SETTING_KEYER_MESSAGE_4:
    case UI_SETTING_KEYER_MESSAGE_5: {
        uint8_t index = (uint8_t)(event->setting - UI_SETTING_KEYER_MESSAGE_1);
        snprintf(config.message[index],
                 sizeof(config.message[index]),
                 "%.*s",
                 (int)KEYER_MESSAGE_MAX_LEN,
                 event->text);
        break;
    }
    case UI_SETTING_NONE:
    case UI_SETTING_VOLUME:
    case UI_SETTING_TONE_HZ:
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
    case UI_SETTING_WORD_MAX_WPM:
    case UI_SETTING_WORD_DELAY_S:
    case UI_SETTING_CALLSIGN_SPEED:
    case UI_SETTING_CALLSIGN_MIN_CHAR_WPM:
    case UI_SETTING_CALLSIGN_MAX_WPM:
    case UI_SETTING_CALLSIGN_DELAY_S:
    case UI_SETTING_PLAINTEXT_CODE_WPM:
    case UI_SETTING_PLAINTEXT_EFFECTIVE_WPM:
    case UI_SETTING_USB_DRIVE:
    case UI_SETTING_KEY_OUT_MODE:
    case UI_SETTING_KEYER_PADDLE_MODE:
    case UI_SETTING_KEYER_MUTE:
    default:
        break;
    }

    keyer_service_set_config(&config);
    app_core_mark_keyer_config_dirty();
    ui_service_refresh();
}

static void app_core_handle_usb_drive_changed(const ui_input_event_t *event)
{
    bool enabled = !storage_usb_drive_is_enabled();

    if (event != NULL && event->setting == UI_SETTING_USB_DRIVE) {
        enabled = event->value != 0;
    }

    /* Storage owns FATFS and USB MSC; app_core only routes the UI request. */
    if (storage_usb_drive_set_enabled(enabled) && !enabled) {
        app_core_load_persisted_settings();
    }
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
        case UI_SETTING_TONE_HZ:
        case UI_SETTING_KEY_IN_WPM:
        case UI_SETTING_KEY_IN_MODE:
        case UI_SETTING_WORD_SPEED:
        case UI_SETTING_WORD_MIN_CHAR_WPM:
        case UI_SETTING_WORD_LESSON:
        case UI_SETTING_WORD_MAX_LEN:
        case UI_SETTING_WORD_MAX_WPM:
        case UI_SETTING_WORD_DELAY_S:
        case UI_SETTING_CALLSIGN_SPEED:
        case UI_SETTING_CALLSIGN_MIN_CHAR_WPM:
        case UI_SETTING_CALLSIGN_MAX_WPM:
        case UI_SETTING_CALLSIGN_DELAY_S:
        case UI_SETTING_PLAINTEXT_CODE_WPM:
        case UI_SETTING_PLAINTEXT_EFFECTIVE_WPM:
        case UI_SETTING_USB_DRIVE:
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
        case UI_SETTING_WORD_MAX_WPM:
            config.max_wpm = (uint8_t)event->value;
            break;
        case UI_SETTING_WORD_DELAY_S:
            config.delay_s = (uint8_t)event->value;
            break;
        case UI_SETTING_NONE:
        case UI_SETTING_VOLUME:
        case UI_SETTING_TONE_HZ:
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
        case UI_SETTING_CALLSIGN_DELAY_S:
        case UI_SETTING_PLAINTEXT_CODE_WPM:
        case UI_SETTING_PLAINTEXT_EFFECTIVE_WPM:
        case UI_SETTING_USB_DRIVE:
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
        case UI_SETTING_CALLSIGN_DELAY_S:
            config.delay_s = (uint8_t)event->value;
            break;
        case UI_SETTING_NONE:
        case UI_SETTING_VOLUME:
        case UI_SETTING_TONE_HZ:
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
        case UI_SETTING_WORD_MAX_WPM:
        case UI_SETTING_WORD_DELAY_S:
        case UI_SETTING_PLAINTEXT_CODE_WPM:
        case UI_SETTING_PLAINTEXT_EFFECTIVE_WPM:
        case UI_SETTING_USB_DRIVE:
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
        case UI_SETTING_TONE_HZ:
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
        case UI_SETTING_WORD_MAX_WPM:
        case UI_SETTING_WORD_DELAY_S:
        case UI_SETTING_CALLSIGN_SPEED:
        case UI_SETTING_CALLSIGN_MIN_CHAR_WPM:
        case UI_SETTING_CALLSIGN_MAX_WPM:
        case UI_SETTING_CALLSIGN_DELAY_S:
        case UI_SETTING_USB_DRIVE:
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
        app_core_keyer_append_tx_char(key);
    }

    ui_service_refresh();
}

static bool app_core_append_training_copy_char(char key)
{
    if (s_app.mode == APP_MODE_PLAINTEXT) {
        return cw_trainer_plaintext_append_char(key);
    }
    if (s_app.mode == APP_MODE_LESSONS) {
        return cw_trainer_lesson_append_char(key);
    }
    if (s_app.mode == APP_MODE_WORDS) {
        return cw_trainer_word_append_char(key);
    }
    if (s_app.mode == APP_MODE_CALLSIGNS) {
        return cw_trainer_callsign_append_char(key);
    }

    return false;
}

static bool app_core_append_training_space(void)
{
    if (s_app.mode == APP_MODE_WORDS || s_app.mode == APP_MODE_CALLSIGNS) {
        return false;
    }

    return app_core_append_training_copy_char(' ');
}

static bool app_core_handle_training_enter_from_keyer(void)
{
    if (s_app.mode == APP_MODE_WORDS) {
        app_core_handle_word_select();
        return false;
    }

    if (s_app.mode == APP_MODE_CALLSIGNS) {
        app_core_handle_callsign_select();
        return false;
    }

    return false;
}

static bool app_core_backspace_training_copy(void)
{
    if (s_app.mode == APP_MODE_PLAINTEXT) {
        const cw_plaintext_view_t *view = cw_trainer_plaintext_get_view();
        if (view != NULL && view->state == CW_PLAINTEXT_STATE_COPYING && view->copy_len > 0U) {
            cw_trainer_plaintext_backspace();
            return true;
        }
    } else if (s_app.mode == APP_MODE_LESSONS) {
        const cw_lesson_view_t *view = cw_trainer_lesson_get_view();
        if (view != NULL && view->state == CW_LESSON_STATE_COPYING && view->copy_len > 0U) {
            cw_trainer_lesson_backspace();
            return true;
        }
    } else if (s_app.mode == APP_MODE_WORDS) {
        const cw_word_view_t *view = cw_trainer_word_get_view();
        if (view != NULL && view->state == CW_WORD_STATE_COPYING && view->copy_len > 0U) {
            cw_trainer_word_backspace();
            return true;
        }
    } else if (s_app.mode == APP_MODE_CALLSIGNS) {
        const cw_callsign_view_t *view = cw_trainer_callsign_get_view();
        if (view != NULL && view->state == CW_CALLSIGN_STATE_COPYING && view->copy_len > 0U) {
            cw_trainer_callsign_backspace();
            return true;
        }
    }

    return false;
}

static bool app_core_handle_keyer_mode_decoded_event(const keyer_event_t *event)
{
    if (s_app.mode != APP_MODE_KEYER || event == NULL) {
        return false;
    }

    switch (event->type) {
    case KEYER_EVENT_CHAR_COMPLETE:
        ui_service_keyer_append_decoded_char(event->decoded_char);
        return true;
    case KEYER_EVENT_WORD_SPACE:
        ui_service_keyer_append_decoded_char(' ');
        return true;
    case KEYER_EVENT_BACKSPACE:
        ui_service_keyer_backspace_decoded();
        return true;
    case KEYER_EVENT_ENTER:
        return false;
    case KEYER_EVENT_DIT:
    case KEYER_EVENT_DAH:
        if (s_keyer.macro_active) {
            app_core_keyer_cancel_macro();
            ui_service_keyer_clear_tx_text();
            return true;
        }
        break;
    case KEYER_EVENT_TIMING_WARNING:
    case KEYER_EVENT_TIMING_ERROR:
    case KEYER_EVENT_NONE:
    default:
        break;
    }

    return false;
}

static void app_core_handle_keyer_event(const keyer_event_t *event)
{
    bool handled = false;

    if (event == NULL || event->type == KEYER_EVENT_NONE) {
        return;
    }

    /* keyer_service owns paddle decoding; app_core only routes decoded intent. */
    if (s_app.mode == APP_MODE_KEYER) {
        handled = app_core_handle_keyer_mode_decoded_event(event);
    } else {
        switch (event->type) {
        case KEYER_EVENT_CHAR_COMPLETE:
            handled = app_core_append_training_copy_char(event->decoded_char);
            break;
        case KEYER_EVENT_WORD_SPACE:
            handled = app_core_append_training_space();
            break;
        case KEYER_EVENT_BACKSPACE:
            handled = app_core_backspace_training_copy();
            break;
        case KEYER_EVENT_ENTER:
            handled = app_core_handle_training_enter_from_keyer();
            break;
        case KEYER_EVENT_DIT:
        case KEYER_EVENT_DAH:
        case KEYER_EVENT_TIMING_WARNING:
        case KEYER_EVENT_TIMING_ERROR:
        case KEYER_EVENT_NONE:
        default:
            break;
        }
    }

    if (handled) {
        ui_service_refresh();
    }
}

static void app_core_drain_keyer_events(void)
{
    for (;;) {
        keyer_event_t event = keyer_service_poll_event();
        if (event.type == KEYER_EVENT_NONE) {
            return;
        }

        app_core_handle_keyer_event(&event);
    }
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
            app_core_keyer_cancel_macro();
            app_core_keyer_clear_tx_buffer();
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
    case UI_INPUT_EVENT_TONE_CHANGED:
        app_core_handle_tone_changed(&event);
        break;
    case UI_INPUT_EVENT_KEY_IN_WPM_CHANGED:
        app_core_handle_key_in_wpm_changed(&event);
        break;
    case UI_INPUT_EVENT_KEY_IN_MODE_CHANGED:
        app_core_handle_key_in_mode_changed(&event);
        break;
    case UI_INPUT_EVENT_KEY_OUT_MODE_CHANGED:
        app_core_handle_key_out_mode_changed(&event);
        break;
    case UI_INPUT_EVENT_KEYER_PADDLE_MODE_CHANGED:
        app_core_handle_keyer_paddle_mode_changed(&event);
        break;
    case UI_INPUT_EVENT_KEYER_CONFIG_CHANGED:
        app_core_handle_keyer_config_changed(&event);
        break;
    case UI_INPUT_EVENT_KEYER_MUTE_CHANGED:
        app_core_handle_keyer_mute_changed(&event);
        break;
    case UI_INPUT_EVENT_KEYER_MACRO_SELECTED:
        app_core_keyer_start_macro((uint8_t)event.value);
        ui_service_refresh();
        break;
    case UI_INPUT_EVENT_KEYER_SHORTCUT_CHANGED:
        if (event.value == 0) {
            app_core_keyer_cancel_macro();
            ui_service_keyer_clear_tx_text();
        }
        ui_service_refresh();
        break;
    case UI_INPUT_EVENT_KEYER_CLEAR:
        if (s_app.mode == APP_MODE_KEYER) {
            app_core_keyer_cancel_macro();
            app_core_keyer_clear_tx_buffer();
            ui_service_keyer_clear_decoded();
            ui_service_refresh();
        }
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
    case UI_INPUT_EVENT_USB_DRIVE_CHANGED:
        app_core_handle_usb_drive_changed(&event);
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
        } else if (s_app.mode == APP_MODE_KEYER) {
            if (s_keyer.macro_pending_start) {
                app_core_keyer_start_pending_macro_now();
            } else {
                app_core_keyer_send_buffer_now();
            }
            ui_service_refresh();
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
        } else if (s_app.mode == APP_MODE_KEYER) {
            app_core_keyer_backspace_tx();
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

    ESP_LOGI(TAG, "init: audio_service");
    audio_service_init();

    ESP_LOGI(TAG, "init: keyer_service");
    keyer_service_init();

    ESP_LOGI(TAG, "init: ui_service");
    ui_service_init();

    ESP_LOGI(TAG, "init: cw_trainer_service");
    cw_trainer_service_init();
    app_core_load_persisted_settings();

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
        cw_trainer_service_update();
        app_core_drain_keyer_events();
        ui_input_event_t event = ui_service_poll_input();
        app_core_handle_ui_event(event);
        app_core_keyer_update();
        app_core_maybe_save_dirty_config();
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

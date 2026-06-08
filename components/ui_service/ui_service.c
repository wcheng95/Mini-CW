/*
 * ui_service
 *
 * Responsibility: Owns Mini-CW UI behavior/state and public UI APIs. Fixed
 * 240x135 drawing is private to ui_screen, and low-level Cardputer
 * display/keyboard access is private to ui_cardputer_port.
 */

#include "ui_service.h"

#include "ui_cardputer_port.h"
#include "ui_screen.h"

/*
 * ui_service may read service state for rendering only.
 * It must not call non-UI mutator APIs; setting changes are emitted as
 * ui_input_event_t and applied by app_core.
 */
#include "audio_service.h"
#include "cw_trainer_service.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "keyer_service.h"
#include "platform_hal.h"
#include "storage_service.h"

#include <stdbool.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ui_service";

typedef enum {
    UI_VIEW_NORMAL = 0,
    UI_VIEW_MODE_SELECT,
    UI_VIEW_MODE_MENU,
} ui_view_t;

typedef enum {
    UI_EDIT_NONE = 0,
    UI_EDIT_VOLUME,
    UI_EDIT_TONE_HZ,
    UI_EDIT_KEY_IN_WPM,
    UI_EDIT_LESSON,
    UI_EDIT_LESSON_DURATION,
    UI_EDIT_LESSON_CODE_WPM,
    UI_EDIT_LESSON_EFFECTIVE_WPM,
    UI_EDIT_LESSON_GROUP_LEN,
    UI_EDIT_WORD_SPEED,
    UI_EDIT_WORD_MIN_CHAR_WPM,
    UI_EDIT_WORD_LESSON,
    UI_EDIT_WORD_MAX_LEN,
    UI_EDIT_WORD_MAX_WPM,
    UI_EDIT_WORD_DELAY_S,
    UI_EDIT_CALLSIGN_SPEED,
    UI_EDIT_CALLSIGN_MIN_CHAR_WPM,
    UI_EDIT_CALLSIGN_MAX_WPM,
    UI_EDIT_CALLSIGN_DELAY_S,
    UI_EDIT_PLAINTEXT_CODE_WPM,
    UI_EDIT_PLAINTEXT_EFFECTIVE_WPM,
    UI_EDIT_KEYER_TX_DELAY_S,
    UI_EDIT_KEYER_TUNE_TIMEOUT_S,
    UI_EDIT_KEYER_REPEAT_INTERVAL_S,
    UI_EDIT_KEYER_SK_WPM,
} ui_edit_target_t;

typedef enum {
    UI_TEXT_EDIT_NONE = 0,
    UI_TEXT_EDIT_KEYER_MESSAGE_1,
    UI_TEXT_EDIT_KEYER_MESSAGE_2,
    UI_TEXT_EDIT_KEYER_MESSAGE_3,
    UI_TEXT_EDIT_KEYER_MESSAGE_4,
    UI_TEXT_EDIT_KEYER_MESSAGE_5,
    UI_TEXT_EDIT_KEYER_MYCALL,
} ui_text_edit_target_t;

typedef enum {
    UI_DATETIME_EDIT_NONE = 0,
    UI_DATETIME_EDIT_DATE,
    UI_DATETIME_EDIT_TIME,
} ui_datetime_edit_target_t;

typedef struct {
    ui_service_mode_t mode;
    ui_view_t view;
    uint8_t menu_page;
    ui_edit_target_t edit_target;
    ui_text_edit_target_t text_edit_target;
    ui_datetime_edit_target_t datetime_edit_target;
    uint8_t edit_item;
    char edit_buf[4];
    char text_edit_buf[KEYER_MESSAGE_MAX_LEN + 1U];
    char datetime_edit_buf[STORAGE_SYSTEM_DATE_LEN + 1U];
    size_t datetime_edit_cursor;
    size_t text_edit_cursor;
    bool text_edit_cursor_repeat_active;
    char text_edit_cursor_repeat_key;
    TickType_t text_edit_cursor_repeat_due;
    bool edit_user_digits;
    bool keyer_shortcut_active;
    uint8_t keyer_shortcut_macro;
    bool keyer_tune_active;
} ui_service_state_t;

static const ui_input_event_t UI_EVENT_NONE = {
    .type = UI_INPUT_EVENT_NONE,
    .key = '\0',
    .setting = UI_SETTING_NONE,
    .value = 0,
    .delta = 0,
    .text = "",
};

static ui_service_state_t s_ui = {
    .mode = UI_SERVICE_MODE_KEYER,
    .view = UI_VIEW_NORMAL,
    .menu_page = 0U,
    .edit_target = UI_EDIT_NONE,
    .text_edit_target = UI_TEXT_EDIT_NONE,
    .datetime_edit_target = UI_DATETIME_EDIT_NONE,
    .edit_item = 0U,
    .edit_buf = "",
    .text_edit_buf = "",
    .datetime_edit_buf = "",
    .datetime_edit_cursor = 0U,
    .text_edit_cursor = 0U,
    .text_edit_cursor_repeat_active = false,
    .text_edit_cursor_repeat_key = '\0',
    .text_edit_cursor_repeat_due = 0,
    .edit_user_digits = false,
    .keyer_shortcut_active = false,
    .keyer_shortcut_macro = 0U,
    .keyer_tune_active = false,
};

static bool s_cardputer_ready;

#define UI_VOLUME_MIN 0
#define UI_VOLUME_MAX 99
#define UI_VOLUME_STEP 5
#define UI_TONE_HZ_MIN 300
#define UI_TONE_HZ_MAX 999
#define UI_TONE_HZ_STEP 50
#define UI_WPM_MIN 5
#define UI_WPM_MAX 60
#define UI_WPM_STEP 1
#define UI_DELAY_S_MIN 0
#define UI_DELAY_S_MAX 5
#define UI_KEYER_TX_DELAY_S_MIN 0
#define UI_KEYER_TUNE_TIMEOUT_S_MIN 0
#define UI_KEYER_TUNE_TIMEOUT_S_MAX 20
#define UI_KEYER_REPEAT_S_MIN 1
#define UI_KEYER_DELAY_S_MAX 99
#define UI_KEYER_VISIBLE_LINES 5U
#define UI_KEYER_HISTORY_LINES 64U
#define UI_KEYER_HISTORY_CAPACITY (UI_KEYER_HISTORY_LINES * UI_COLS)
#define UI_TEXT_CURSOR_REPEAT_DELAY_MS 400U
#define UI_TEXT_CURSOR_REPEAT_INTERVAL_MS 100U
#define UI_SYSTEM_DEFAULT_DATE "2026-01-01"
#define UI_SYSTEM_DEFAULT_TIME "00:00:00"

static char s_keyer_history[UI_KEYER_HISTORY_CAPACITY + 1U];
static uint16_t s_keyer_history_len;
static uint16_t s_keyer_history_scroll_top;
static bool s_keyer_history_follow_tail = true;
static char s_keyer_tx_text[UI_INPUT_EVENT_TEXT_MAX + 1U];
static char s_keyer_status_text[UI_INPUT_EVENT_TEXT_MAX + 1U];
static TickType_t s_keyer_status_until_tick;

static void ui_service_render_current_view(void);
static void ui_service_set_event(ui_input_event_t *out_event,
                                 ui_input_event_type_t type,
                                 char key);

static bool ui_service_mode_is_valid(ui_service_mode_t mode)
{
    return mode >= UI_SERVICE_MODE_PLAINTEXT && mode <= UI_SERVICE_MODE_SYSTEM;
}

static const char *ui_service_mode_name(ui_service_mode_t mode)
{
    switch (mode) {
    case UI_SERVICE_MODE_PLAINTEXT:
        return "Plain";
    case UI_SERVICE_MODE_KEYER:
        return "Keyer";
    case UI_SERVICE_MODE_LESSONS:
        return "Lessons";
    case UI_SERVICE_MODE_WORDS:
        return "Words";
    case UI_SERVICE_MODE_CALLSIGNS:
        return "Calls";
    case UI_SERVICE_MODE_SYSTEM:
        return "System";
    default:
        return "Unknown";
    }
}

static void ui_service_set_text(char *dest, size_t dest_size, const char *text)
{
    if (dest == NULL || dest_size == 0U) {
        return;
    }

    snprintf(dest, dest_size, "%s", text ? text : "");
}

static void ui_service_set_top_chars(mini_cw_screen_t *screen,
                                     const char *text,
                                     mini_cw_screen_color_t color)
{
    size_t i = 0U;

    if (screen == NULL) {
        return;
    }

    memset(screen->top, ' ', UI_COLS);
    screen->top[UI_COLS] = '\0';
    for (i = 0U; i < UI_COLS; ++i) {
        screen->top_color[i] = color;
    }

    if (text == NULL) {
        return;
    }

    for (i = 0U; i < UI_COLS && text[i] != '\0'; ++i) {
        screen->top[i] = text[i];
    }
}

static int ui_service_clamp_int(int value, int min_value, int max_value)
{
    if (value < min_value) {
        return min_value;
    }

    if (value > max_value) {
        return max_value;
    }

    return value;
}

static bool ui_service_tick_reached(TickType_t now, TickType_t due)
{
    return (TickType_t)(now - due) < (TickType_t)(UINT32_MAX / 2U);
}

static bool ui_service_is_editing_item(uint8_t item)
{
    return s_ui.edit_target != UI_EDIT_NONE && s_ui.edit_item == item;
}

static bool ui_service_is_text_editing(void)
{
    return s_ui.text_edit_target != UI_TEXT_EDIT_NONE;
}

static bool ui_service_is_datetime_editing(void)
{
    return s_ui.datetime_edit_target != UI_DATETIME_EDIT_NONE;
}

static void ui_service_reset_text_cursor_repeat(void)
{
    s_ui.text_edit_cursor_repeat_active = false;
    s_ui.text_edit_cursor_repeat_key = '\0';
    s_ui.text_edit_cursor_repeat_due = 0;
}

static size_t ui_service_text_edit_len(void)
{
    return strlen(s_ui.text_edit_buf);
}

static size_t ui_service_text_edit_max_len(void)
{
    if (s_ui.text_edit_target == UI_TEXT_EDIT_KEYER_MYCALL) {
        return KEYER_MYCALL_MAX_LEN;
    }

    return KEYER_MESSAGE_MAX_LEN;
}

static bool ui_service_keyer_mycall_char(char ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '/';
}

static void ui_service_clamp_text_cursor(void)
{
    size_t len = ui_service_text_edit_len();

    if (s_ui.text_edit_cursor > len) {
        s_ui.text_edit_cursor = len;
    }
}

static void ui_service_move_text_cursor(int delta)
{
    size_t len;

    ui_service_clamp_text_cursor();
    len = ui_service_text_edit_len();

    if (delta < 0) {
        if (s_ui.text_edit_cursor > 0U) {
            --s_ui.text_edit_cursor;
        }
    } else if (delta > 0) {
        if (s_ui.text_edit_cursor < len) {
            ++s_ui.text_edit_cursor;
        }
    }
}

static void ui_service_clear_edit(void)
{
    s_ui.edit_target = UI_EDIT_NONE;
    s_ui.text_edit_target = UI_TEXT_EDIT_NONE;
    s_ui.datetime_edit_target = UI_DATETIME_EDIT_NONE;
    s_ui.edit_item = 0U;
    s_ui.edit_buf[0] = '\0';
    s_ui.text_edit_buf[0] = '\0';
    s_ui.datetime_edit_buf[0] = '\0';
    s_ui.datetime_edit_cursor = 0U;
    s_ui.text_edit_cursor = 0U;
    ui_service_reset_text_cursor_repeat();
    s_ui.edit_user_digits = false;
}

static void ui_service_prepare_screen(mini_cw_screen_t *screen)
{
    if (screen == NULL) {
        return;
    }

    memset(screen, 0, sizeof(*screen));
    ui_service_set_text(screen->mode, sizeof(screen->mode), ui_service_mode_name(s_ui.mode));
    ui_service_set_top_chars(screen, ui_service_mode_name(s_ui.mode), MINI_CW_SCREEN_COLOR_WHITE);
}

static uint16_t ui_service_keyer_history_line_count(void)
{
    if (s_keyer_history_len == 0U) {
        return 1U;
    }

    return (uint16_t)((s_keyer_history_len + UI_COLS - 1U) / UI_COLS);
}

static uint16_t ui_service_keyer_history_max_scroll_top(void)
{
    uint16_t line_count = ui_service_keyer_history_line_count();

    if (line_count <= UI_KEYER_VISIBLE_LINES) {
        return 0U;
    }

    return (uint16_t)(line_count - UI_KEYER_VISIBLE_LINES);
}

static void ui_service_keyer_clamp_scroll(void)
{
    uint16_t max_scroll_top = ui_service_keyer_history_max_scroll_top();

    if (s_keyer_history_scroll_top > max_scroll_top) {
        s_keyer_history_scroll_top = max_scroll_top;
    }
}

static void ui_service_keyer_follow_latest(void)
{
    s_keyer_history_scroll_top = ui_service_keyer_history_max_scroll_top();
    s_keyer_history_follow_tail = true;
}

static void ui_service_keyer_scroll_decoded(int delta_lines)
{
    uint16_t max_scroll_top = ui_service_keyer_history_max_scroll_top();
    int next = (int)s_keyer_history_scroll_top + delta_lines;

    if (next < 0) {
        next = 0;
    }
    if (next > (int)max_scroll_top) {
        next = (int)max_scroll_top;
    }

    s_keyer_history_scroll_top = (uint16_t)next;
    s_keyer_history_follow_tail = s_keyer_history_scroll_top >= max_scroll_top;
}

static void ui_service_keyer_render_history_line(char *dest,
                                                 size_t dest_size,
                                                 uint16_t line_index)
{
    uint16_t start = (uint16_t)(line_index * UI_COLS);
    uint16_t count;

    if (dest == NULL || dest_size == 0U) {
        return;
    }

    dest[0] = '\0';
    if (start >= s_keyer_history_len) {
        return;
    }

    count = (uint16_t)(s_keyer_history_len - start);
    if (count > UI_COLS) {
        count = UI_COLS;
    }
    if (count + 1U > dest_size) {
        count = (uint16_t)(dest_size - 1U);
    }

    memcpy(dest, &s_keyer_history[start], count);
    dest[count] = '\0';
}

void ui_service_keyer_append_decoded_char(char ch)
{
    if (ch < 32 || ch > 126) {
        return;
    }

    if (s_keyer_history_len >= UI_KEYER_HISTORY_CAPACITY) {
        memmove(s_keyer_history,
                s_keyer_history + UI_COLS,
                UI_KEYER_HISTORY_CAPACITY - UI_COLS);
        s_keyer_history_len = UI_KEYER_HISTORY_CAPACITY - UI_COLS;
        s_keyer_history[s_keyer_history_len] = '\0';
        if (s_keyer_history_scroll_top > 0U) {
            --s_keyer_history_scroll_top;
        }
    }

    s_keyer_history[s_keyer_history_len] = ch;
    ++s_keyer_history_len;
    s_keyer_history[s_keyer_history_len] = '\0';
    ui_service_keyer_clamp_scroll();
    if (s_keyer_history_follow_tail) {
        ui_service_keyer_follow_latest();
    }
}

void ui_service_keyer_backspace_decoded(void)
{
    if (s_keyer_history_len == 0U) {
        return;
    }

    --s_keyer_history_len;
    s_keyer_history[s_keyer_history_len] = '\0';
    ui_service_keyer_clamp_scroll();
    if (s_keyer_history_follow_tail) {
        ui_service_keyer_follow_latest();
    }
}

void ui_service_keyer_clear_decoded(void)
{
    s_keyer_history_len = 0U;
    s_keyer_history[0] = '\0';
    s_keyer_history_scroll_top = 0U;
    s_keyer_history_follow_tail = true;
}

void ui_service_keyer_set_tx_text(const char *text)
{
    snprintf(s_keyer_tx_text, sizeof(s_keyer_tx_text), "%s", text ? text : "");
}

void ui_service_keyer_set_status(const char *text)
{
    snprintf(s_keyer_status_text, sizeof(s_keyer_status_text), "%s", text ? text : "");
    s_keyer_status_until_tick = xTaskGetTickCount() + pdMS_TO_TICKS(1200);
}

bool ui_service_keyer_shortcut_active(void)
{
    return s_ui.keyer_shortcut_active;
}

void ui_service_keyer_set_tune_active(bool active)
{
    s_ui.keyer_tune_active = active;
    if (active) {
        s_ui.keyer_shortcut_active = false;
        s_ui.keyer_shortcut_macro = 0U;
        ui_service_clear_edit();
    }
}

bool ui_service_keyer_tune_active(void)
{
    return s_ui.keyer_tune_active;
}

static int ui_service_read_battery_percent(void)
{
    int percent = 0;

    if (platform_hal_get_battery_percent(&percent) != ESP_OK) {
        return 0;
    }

    return ui_service_clamp_int(percent, 0, 100);
}

static int ui_service_edit_min(ui_edit_target_t target)
{
    switch (target) {
    case UI_EDIT_VOLUME:
        return UI_VOLUME_MIN;
    case UI_EDIT_TONE_HZ:
        return UI_TONE_HZ_MIN;
    case UI_EDIT_KEY_IN_WPM:
        return UI_WPM_MIN;
    case UI_EDIT_LESSON:
        return 1;
    case UI_EDIT_LESSON_DURATION:
        return 1;
    case UI_EDIT_LESSON_CODE_WPM:
    case UI_EDIT_LESSON_EFFECTIVE_WPM:
        return UI_WPM_MIN;
    case UI_EDIT_LESSON_GROUP_LEN:
        return 0;
    case UI_EDIT_WORD_SPEED:
    case UI_EDIT_WORD_MIN_CHAR_WPM:
        return UI_WPM_MIN;
    case UI_EDIT_WORD_LESSON:
        return 9;
    case UI_EDIT_WORD_MAX_LEN:
        return 2;
    case UI_EDIT_WORD_MAX_WPM:
        return UI_WPM_MIN;
    case UI_EDIT_WORD_DELAY_S:
        return UI_DELAY_S_MIN;
    case UI_EDIT_CALLSIGN_SPEED:
    case UI_EDIT_CALLSIGN_MIN_CHAR_WPM:
    case UI_EDIT_CALLSIGN_MAX_WPM:
        return UI_WPM_MIN;
    case UI_EDIT_CALLSIGN_DELAY_S:
        return UI_DELAY_S_MIN;
    case UI_EDIT_PLAINTEXT_CODE_WPM:
    case UI_EDIT_PLAINTEXT_EFFECTIVE_WPM:
    case UI_EDIT_KEYER_SK_WPM:
        return UI_WPM_MIN;
    case UI_EDIT_KEYER_TX_DELAY_S:
        return UI_KEYER_TX_DELAY_S_MIN;
    case UI_EDIT_KEYER_TUNE_TIMEOUT_S:
        return UI_KEYER_TUNE_TIMEOUT_S_MIN;
    case UI_EDIT_KEYER_REPEAT_INTERVAL_S:
        return UI_KEYER_REPEAT_S_MIN;
    case UI_EDIT_NONE:
    default:
        return 0;
    }
}

static int ui_service_edit_max(ui_edit_target_t target)
{
    switch (target) {
    case UI_EDIT_VOLUME:
        return UI_VOLUME_MAX;
    case UI_EDIT_TONE_HZ:
        return UI_TONE_HZ_MAX;
    case UI_EDIT_KEY_IN_WPM:
    case UI_EDIT_KEYER_SK_WPM:
        return UI_WPM_MAX;
    case UI_EDIT_LESSON:
        return 40;
    case UI_EDIT_LESSON_DURATION:
        return 5;
    case UI_EDIT_LESSON_CODE_WPM:
    case UI_EDIT_LESSON_EFFECTIVE_WPM:
        return 40;
    case UI_EDIT_LESSON_GROUP_LEN:
        return 7;
    case UI_EDIT_WORD_SPEED:
    case UI_EDIT_WORD_MIN_CHAR_WPM:
        return 40;
    case UI_EDIT_WORD_LESSON:
        return 40;
    case UI_EDIT_WORD_MAX_LEN:
        return 15;
    case UI_EDIT_WORD_MAX_WPM:
        return 40;
    case UI_EDIT_WORD_DELAY_S:
        return UI_DELAY_S_MAX;
    case UI_EDIT_CALLSIGN_SPEED:
    case UI_EDIT_CALLSIGN_MIN_CHAR_WPM:
    case UI_EDIT_CALLSIGN_MAX_WPM:
        return 40;
    case UI_EDIT_CALLSIGN_DELAY_S:
        return UI_DELAY_S_MAX;
    case UI_EDIT_PLAINTEXT_CODE_WPM:
    case UI_EDIT_PLAINTEXT_EFFECTIVE_WPM:
        return 40;
    case UI_EDIT_KEYER_TX_DELAY_S:
    case UI_EDIT_KEYER_REPEAT_INTERVAL_S:
        return UI_KEYER_DELAY_S_MAX;
    case UI_EDIT_KEYER_TUNE_TIMEOUT_S:
        return UI_KEYER_TUNE_TIMEOUT_S_MAX;
    case UI_EDIT_NONE:
    default:
        return 0;
    }
}

static int ui_service_edit_step(ui_edit_target_t target)
{
    switch (target) {
    case UI_EDIT_VOLUME:
        return UI_VOLUME_STEP;
    case UI_EDIT_TONE_HZ:
        return UI_TONE_HZ_STEP;
    case UI_EDIT_KEY_IN_WPM:
        return UI_WPM_STEP;
    case UI_EDIT_LESSON:
    case UI_EDIT_LESSON_DURATION:
    case UI_EDIT_LESSON_CODE_WPM:
    case UI_EDIT_LESSON_EFFECTIVE_WPM:
    case UI_EDIT_LESSON_GROUP_LEN:
    case UI_EDIT_WORD_SPEED:
    case UI_EDIT_WORD_MIN_CHAR_WPM:
    case UI_EDIT_WORD_LESSON:
    case UI_EDIT_WORD_MAX_LEN:
    case UI_EDIT_WORD_MAX_WPM:
    case UI_EDIT_WORD_DELAY_S:
    case UI_EDIT_CALLSIGN_SPEED:
    case UI_EDIT_CALLSIGN_MIN_CHAR_WPM:
    case UI_EDIT_CALLSIGN_MAX_WPM:
    case UI_EDIT_CALLSIGN_DELAY_S:
    case UI_EDIT_PLAINTEXT_CODE_WPM:
    case UI_EDIT_PLAINTEXT_EFFECTIVE_WPM:
    case UI_EDIT_KEYER_SK_WPM:
    case UI_EDIT_KEYER_TX_DELAY_S:
    case UI_EDIT_KEYER_TUNE_TIMEOUT_S:
    case UI_EDIT_KEYER_REPEAT_INTERVAL_S:
        return 1;
    case UI_EDIT_NONE:
    default:
        return 1;
    }
}

static size_t ui_service_edit_max_digits(ui_edit_target_t target)
{
    if (target == UI_EDIT_LESSON_DURATION || target == UI_EDIT_LESSON_GROUP_LEN ||
        target == UI_EDIT_WORD_DELAY_S || target == UI_EDIT_CALLSIGN_DELAY_S) {
        return 1U;
    }

    if (target == UI_EDIT_TONE_HZ) {
        return 3U;
    }

    return 2U;
}

static int ui_service_get_edit_value(ui_edit_target_t target)
{
    switch (target) {
    case UI_EDIT_VOLUME:
        return audio_service_get_volume();
    case UI_EDIT_TONE_HZ:
        return audio_service_get_tone_hz();
    case UI_EDIT_KEY_IN_WPM:
        return keyer_service_get_key_in_wpm();
    case UI_EDIT_LESSON:
        return cw_trainer_lesson_get_config()->lesson;
    case UI_EDIT_LESSON_DURATION:
        return cw_trainer_lesson_get_config()->duration_min;
    case UI_EDIT_LESSON_CODE_WPM:
        return cw_trainer_lesson_get_config()->code_wpm;
    case UI_EDIT_LESSON_EFFECTIVE_WPM:
        return cw_trainer_lesson_get_config()->effective_wpm;
    case UI_EDIT_LESSON_GROUP_LEN:
        return cw_trainer_lesson_get_config()->group_len;
    case UI_EDIT_WORD_SPEED:
        return cw_trainer_word_get_config()->start_wpm;
    case UI_EDIT_WORD_MIN_CHAR_WPM:
        return cw_trainer_word_get_config()->min_char_wpm;
    case UI_EDIT_WORD_LESSON:
        return cw_trainer_word_get_config()->lesson;
    case UI_EDIT_WORD_MAX_LEN:
        return cw_trainer_word_get_config()->max_word_len;
    case UI_EDIT_WORD_MAX_WPM:
        return cw_trainer_word_get_config()->max_wpm;
    case UI_EDIT_WORD_DELAY_S:
        return cw_trainer_word_get_config()->delay_s;
    case UI_EDIT_CALLSIGN_SPEED:
        return cw_trainer_callsign_get_config()->start_wpm;
    case UI_EDIT_CALLSIGN_MIN_CHAR_WPM:
        return cw_trainer_callsign_get_config()->min_char_wpm;
    case UI_EDIT_CALLSIGN_MAX_WPM:
        return cw_trainer_callsign_get_config()->max_wpm;
    case UI_EDIT_CALLSIGN_DELAY_S:
        return cw_trainer_callsign_get_config()->delay_s;
    case UI_EDIT_PLAINTEXT_CODE_WPM:
        return cw_trainer_plaintext_get_config()->code_wpm;
    case UI_EDIT_PLAINTEXT_EFFECTIVE_WPM:
        return cw_trainer_plaintext_get_config()->effective_wpm;
    case UI_EDIT_KEYER_TX_DELAY_S:
        return keyer_service_get_tx_delay_s();
    case UI_EDIT_KEYER_TUNE_TIMEOUT_S:
        return keyer_service_get_tune_timeout_s();
    case UI_EDIT_KEYER_REPEAT_INTERVAL_S:
        return keyer_service_get_repeat_interval_s();
    case UI_EDIT_KEYER_SK_WPM:
        return keyer_service_get_sk_wpm();
    case UI_EDIT_NONE:
    default:
        return 0;
    }
}

static ui_input_event_type_t ui_service_edit_event_type(ui_edit_target_t target)
{
    switch (target) {
    case UI_EDIT_VOLUME:
        return UI_INPUT_EVENT_VOLUME_CHANGED;
    case UI_EDIT_TONE_HZ:
        return UI_INPUT_EVENT_TONE_CHANGED;
    case UI_EDIT_KEY_IN_WPM:
        return UI_INPUT_EVENT_KEY_IN_WPM_CHANGED;
    case UI_EDIT_LESSON:
    case UI_EDIT_LESSON_DURATION:
    case UI_EDIT_LESSON_CODE_WPM:
    case UI_EDIT_LESSON_EFFECTIVE_WPM:
    case UI_EDIT_LESSON_GROUP_LEN:
        return UI_INPUT_EVENT_LESSON_CONFIG_CHANGED;
    case UI_EDIT_WORD_SPEED:
    case UI_EDIT_WORD_MIN_CHAR_WPM:
    case UI_EDIT_WORD_LESSON:
    case UI_EDIT_WORD_MAX_LEN:
    case UI_EDIT_WORD_MAX_WPM:
    case UI_EDIT_WORD_DELAY_S:
        return UI_INPUT_EVENT_WORD_CONFIG_CHANGED;
    case UI_EDIT_CALLSIGN_SPEED:
    case UI_EDIT_CALLSIGN_MIN_CHAR_WPM:
    case UI_EDIT_CALLSIGN_MAX_WPM:
    case UI_EDIT_CALLSIGN_DELAY_S:
        return UI_INPUT_EVENT_CALLSIGN_CONFIG_CHANGED;
    case UI_EDIT_PLAINTEXT_CODE_WPM:
    case UI_EDIT_PLAINTEXT_EFFECTIVE_WPM:
        return UI_INPUT_EVENT_PLAINTEXT_CONFIG_CHANGED;
    case UI_EDIT_KEYER_TX_DELAY_S:
    case UI_EDIT_KEYER_TUNE_TIMEOUT_S:
    case UI_EDIT_KEYER_REPEAT_INTERVAL_S:
    case UI_EDIT_KEYER_SK_WPM:
        return UI_INPUT_EVENT_KEYER_CONFIG_CHANGED;
    case UI_EDIT_NONE:
    default:
        return UI_INPUT_EVENT_NONE;
    }
}

static ui_setting_target_t ui_service_edit_setting_target(ui_edit_target_t target)
{
    switch (target) {
    case UI_EDIT_VOLUME:
        return UI_SETTING_VOLUME;
    case UI_EDIT_TONE_HZ:
        return UI_SETTING_TONE_HZ;
    case UI_EDIT_KEY_IN_WPM:
        return UI_SETTING_KEY_IN_WPM;
    case UI_EDIT_LESSON:
        return UI_SETTING_LESSON;
    case UI_EDIT_LESSON_DURATION:
        return UI_SETTING_LESSON_DURATION;
    case UI_EDIT_LESSON_CODE_WPM:
        return UI_SETTING_LESSON_CODE_WPM;
    case UI_EDIT_LESSON_EFFECTIVE_WPM:
        return UI_SETTING_LESSON_EFFECTIVE_WPM;
    case UI_EDIT_LESSON_GROUP_LEN:
        return UI_SETTING_LESSON_GROUP_LEN;
    case UI_EDIT_WORD_SPEED:
        return UI_SETTING_WORD_SPEED;
    case UI_EDIT_WORD_MIN_CHAR_WPM:
        return UI_SETTING_WORD_MIN_CHAR_WPM;
    case UI_EDIT_WORD_LESSON:
        return UI_SETTING_WORD_LESSON;
    case UI_EDIT_WORD_MAX_LEN:
        return UI_SETTING_WORD_MAX_LEN;
    case UI_EDIT_WORD_MAX_WPM:
        return UI_SETTING_WORD_MAX_WPM;
    case UI_EDIT_WORD_DELAY_S:
        return UI_SETTING_WORD_DELAY_S;
    case UI_EDIT_CALLSIGN_SPEED:
        return UI_SETTING_CALLSIGN_SPEED;
    case UI_EDIT_CALLSIGN_MIN_CHAR_WPM:
        return UI_SETTING_CALLSIGN_MIN_CHAR_WPM;
    case UI_EDIT_CALLSIGN_MAX_WPM:
        return UI_SETTING_CALLSIGN_MAX_WPM;
    case UI_EDIT_CALLSIGN_DELAY_S:
        return UI_SETTING_CALLSIGN_DELAY_S;
    case UI_EDIT_PLAINTEXT_CODE_WPM:
        return UI_SETTING_PLAINTEXT_CODE_WPM;
    case UI_EDIT_PLAINTEXT_EFFECTIVE_WPM:
        return UI_SETTING_PLAINTEXT_EFFECTIVE_WPM;
    case UI_EDIT_KEYER_TX_DELAY_S:
        return UI_SETTING_KEYER_TX_DELAY_S;
    case UI_EDIT_KEYER_TUNE_TIMEOUT_S:
        return UI_SETTING_KEYER_TUNE_TIMEOUT_S;
    case UI_EDIT_KEYER_REPEAT_INTERVAL_S:
        return UI_SETTING_KEYER_REPEAT_INTERVAL_S;
    case UI_EDIT_KEYER_SK_WPM:
        return UI_SETTING_KEYER_SK_WPM;
    case UI_EDIT_NONE:
    default:
        return UI_SETTING_NONE;
    }
}

static void ui_service_set_edit_buf_value(int value)
{
    if (s_ui.edit_target == UI_EDIT_NONE) {
        s_ui.edit_buf[0] = '\0';
        return;
    }

    value = ui_service_clamp_int(value,
                                 ui_service_edit_min(s_ui.edit_target),
                                 ui_service_edit_max(s_ui.edit_target));
    snprintf(s_ui.edit_buf, sizeof(s_ui.edit_buf), "%d", value);
}

static void ui_service_begin_numeric_edit(uint8_t item, ui_edit_target_t target)
{
    s_ui.edit_target = target;
    s_ui.text_edit_target = UI_TEXT_EDIT_NONE;
    s_ui.datetime_edit_target = UI_DATETIME_EDIT_NONE;
    s_ui.edit_item = item;
    s_ui.edit_user_digits = false;
    ui_service_set_edit_buf_value(ui_service_get_edit_value(target));
    ESP_LOGI(TAG, "mode menu edit started: item=%u", s_ui.edit_item);
}

static int ui_service_edit_buffer_value(void)
{
    if (s_ui.edit_target == UI_EDIT_NONE) {
        return 0;
    }

    if (s_ui.edit_buf[0] == '\0') {
        return ui_service_get_edit_value(s_ui.edit_target);
    }

    return atoi(s_ui.edit_buf);
}

static ui_edit_target_t ui_service_commit_numeric_edit(ui_input_event_t *out_event, char key)
{
    ui_edit_target_t target = s_ui.edit_target;
    int value;

    if (target == UI_EDIT_NONE) {
        return UI_EDIT_NONE;
    }

    value = ui_service_edit_buffer_value();
    value = ui_service_clamp_int(value, ui_service_edit_min(target), ui_service_edit_max(target));
    ui_service_set_event(out_event, ui_service_edit_event_type(target), key);
    if (out_event != NULL) {
        out_event->setting = ui_service_edit_setting_target(target);
        out_event->value = value;
        out_event->delta = 0;
    }
    ESP_LOGI(TAG, "mode menu edit committed: value=%d", value);
    ui_service_clear_edit();
    return target;
}

static ui_edit_target_t ui_service_step_numeric_edit(int delta,
                                                     ui_input_event_t *out_event,
                                                     char key)
{
    int step;
    int value;
    ui_edit_target_t target = s_ui.edit_target;

    if (s_ui.edit_target == UI_EDIT_NONE) {
        return UI_EDIT_NONE;
    }

    step = ui_service_edit_step(s_ui.edit_target);
    value = ui_service_get_edit_value(s_ui.edit_target) + delta * step;
    value = ui_service_clamp_int(value,
                                 ui_service_edit_min(s_ui.edit_target),
                                 ui_service_edit_max(s_ui.edit_target));

    ui_service_set_edit_buf_value(value);
    ui_service_set_event(out_event, ui_service_edit_event_type(target), key);
    if (out_event != NULL) {
        out_event->setting = ui_service_edit_setting_target(target);
        out_event->value = value;
        out_event->delta = delta * step;
    }

    s_ui.edit_user_digits = false;
    return target;
}

static void ui_service_append_edit_digit(char digit)
{
    size_t len;

    if (s_ui.edit_target == UI_EDIT_NONE || digit < '0' || digit > '9') {
        return;
    }

    if (!s_ui.edit_user_digits) {
        s_ui.edit_buf[0] = '\0';
        s_ui.edit_user_digits = true;
    }

    len = strlen(s_ui.edit_buf);
    if (len >= ui_service_edit_max_digits(s_ui.edit_target) || len + 1U >= sizeof(s_ui.edit_buf)) {
        return;
    }

    s_ui.edit_buf[len] = digit;
    s_ui.edit_buf[len + 1U] = '\0';
}

static void ui_service_backspace_edit_digit(void)
{
    size_t len;

    if (s_ui.edit_target == UI_EDIT_NONE) {
        return;
    }

    s_ui.edit_user_digits = true;
    len = strlen(s_ui.edit_buf);
    if (len > 0U) {
        s_ui.edit_buf[len - 1U] = '\0';
    }
}

static ui_text_edit_target_t ui_service_message_text_target(uint8_t index)
{
    switch (index) {
    case 0U:
        return UI_TEXT_EDIT_KEYER_MESSAGE_1;
    case 1U:
        return UI_TEXT_EDIT_KEYER_MESSAGE_2;
    case 2U:
        return UI_TEXT_EDIT_KEYER_MESSAGE_3;
    case 3U:
        return UI_TEXT_EDIT_KEYER_MESSAGE_4;
    case 4U:
        return UI_TEXT_EDIT_KEYER_MESSAGE_5;
    default:
        return UI_TEXT_EDIT_NONE;
    }
}

static uint8_t ui_service_text_edit_message_index(ui_text_edit_target_t target)
{
    switch (target) {
    case UI_TEXT_EDIT_KEYER_MESSAGE_1:
        return 0U;
    case UI_TEXT_EDIT_KEYER_MESSAGE_2:
        return 1U;
    case UI_TEXT_EDIT_KEYER_MESSAGE_3:
        return 2U;
    case UI_TEXT_EDIT_KEYER_MESSAGE_4:
        return 3U;
    case UI_TEXT_EDIT_KEYER_MESSAGE_5:
        return 4U;
    case UI_TEXT_EDIT_NONE:
    default:
        return UINT8_MAX;
    }
}

static ui_setting_target_t ui_service_text_edit_setting_target(ui_text_edit_target_t target)
{
    switch (target) {
    case UI_TEXT_EDIT_KEYER_MESSAGE_1:
        return UI_SETTING_KEYER_MESSAGE_1;
    case UI_TEXT_EDIT_KEYER_MESSAGE_2:
        return UI_SETTING_KEYER_MESSAGE_2;
    case UI_TEXT_EDIT_KEYER_MESSAGE_3:
        return UI_SETTING_KEYER_MESSAGE_3;
    case UI_TEXT_EDIT_KEYER_MESSAGE_4:
        return UI_SETTING_KEYER_MESSAGE_4;
    case UI_TEXT_EDIT_KEYER_MESSAGE_5:
        return UI_SETTING_KEYER_MESSAGE_5;
    case UI_TEXT_EDIT_KEYER_MYCALL:
        return UI_SETTING_KEYER_MYCALL;
    case UI_TEXT_EDIT_NONE:
    default:
        return UI_SETTING_NONE;
    }
}

static void ui_service_begin_text_edit(uint8_t item, ui_text_edit_target_t target)
{
    uint8_t index = ui_service_text_edit_message_index(target);
    const char *text = NULL;

    if (target == UI_TEXT_EDIT_NONE) {
        return;
    }

    if (target == UI_TEXT_EDIT_KEYER_MYCALL) {
        text = keyer_service_get_mycall();
    } else if (index < KEYER_MESSAGE_COUNT) {
        text = keyer_service_get_message(index);
    } else {
        return;
    }

    s_ui.text_edit_target = target;
    s_ui.edit_target = UI_EDIT_NONE;
    s_ui.datetime_edit_target = UI_DATETIME_EDIT_NONE;
    s_ui.edit_item = item;
    snprintf(s_ui.text_edit_buf,
             sizeof(s_ui.text_edit_buf),
             "%s",
             text);
    s_ui.text_edit_cursor = strlen(s_ui.text_edit_buf);
    ui_service_reset_text_cursor_repeat();
    ESP_LOGI(TAG, "text edit started: item=%u", (unsigned)item);
}

static bool ui_service_commit_text_edit(ui_input_event_t *out_event, char key)
{
    ui_text_edit_target_t target = s_ui.text_edit_target;

    if (target == UI_TEXT_EDIT_NONE) {
        return false;
    }

    ui_service_set_event(out_event, UI_INPUT_EVENT_KEYER_CONFIG_CHANGED, key);
    if (out_event != NULL) {
        out_event->setting = ui_service_text_edit_setting_target(target);
        snprintf(out_event->text, sizeof(out_event->text), "%s", s_ui.text_edit_buf);
    }

    ESP_LOGI(TAG, "text edit committed");
    ui_service_clear_edit();
    return true;
}

static bool ui_service_handle_text_edit_char(char key, ui_input_event_t *out_event)
{
    size_t len;
    char normalized;

    if (s_ui.text_edit_target == UI_TEXT_EDIT_NONE) {
        return false;
    }

    if (key == '\n' || key == '\r') {
        return ui_service_commit_text_edit(out_event, key);
    }

    if (key == '`' || key == '\x1B') {
        ui_service_clear_edit();
        return true;
    }

    if (key == '\b' || key == 0x7f) {
        ui_service_clamp_text_cursor();
        len = strlen(s_ui.text_edit_buf);
        if (s_ui.text_edit_cursor > 0U) {
            memmove(&s_ui.text_edit_buf[s_ui.text_edit_cursor - 1U],
                    &s_ui.text_edit_buf[s_ui.text_edit_cursor],
                    len - s_ui.text_edit_cursor + 1U);
            --s_ui.text_edit_cursor;
        }
        return true;
    }

    if (key < 32 || key > 126) {
        return true;
    }

    normalized = (char)toupper((unsigned char)key);
    if (s_ui.text_edit_target == UI_TEXT_EDIT_KEYER_MYCALL &&
        !ui_service_keyer_mycall_char(normalized)) {
        return true;
    }

    len = strlen(s_ui.text_edit_buf);
    if (len >= ui_service_text_edit_max_len() || len + 1U >= sizeof(s_ui.text_edit_buf)) {
        return true;
    }

    ui_service_clamp_text_cursor();
    memmove(&s_ui.text_edit_buf[s_ui.text_edit_cursor + 1U],
            &s_ui.text_edit_buf[s_ui.text_edit_cursor],
            len - s_ui.text_edit_cursor + 1U);
    s_ui.text_edit_buf[s_ui.text_edit_cursor] = normalized;
    ++s_ui.text_edit_cursor;
    return true;
}

static const char *ui_service_time_source_suffix(platform_hal_time_source_t source)
{
    switch (source) {
    case PLATFORM_HAL_TIME_SOURCE_DS3231:
        return " R";
    case PLATFORM_HAL_TIME_SOURCE_GPS:
        return " G";
    case PLATFORM_HAL_TIME_SOURCE_SOFTWARE:
    default:
        return "";
    }
}

static void ui_service_format_datetime_date(const platform_hal_datetime_t *datetime,
                                            char *dest,
                                            size_t dest_size)
{
    if (dest == NULL || dest_size == 0U) {
        return;
    }

    if (datetime == NULL) {
        snprintf(dest, dest_size, "%s", UI_SYSTEM_DEFAULT_DATE);
        return;
    }

    snprintf(dest,
             dest_size,
             "%04u-%02u-%02u",
             (unsigned)datetime->year,
             (unsigned)datetime->month,
             (unsigned)datetime->day);
}

static void ui_service_format_datetime_time(const platform_hal_datetime_t *datetime,
                                            char *dest,
                                            size_t dest_size)
{
    if (dest == NULL || dest_size == 0U) {
        return;
    }

    if (datetime == NULL) {
        snprintf(dest, dest_size, "%s", UI_SYSTEM_DEFAULT_TIME);
        return;
    }

    snprintf(dest,
             dest_size,
             "%02u:%02u:%02u",
             (unsigned)datetime->hour,
             (unsigned)datetime->minute,
             (unsigned)datetime->second);
}

static size_t ui_service_datetime_edit_len(void)
{
    if (s_ui.datetime_edit_target == UI_DATETIME_EDIT_DATE) {
        return STORAGE_SYSTEM_DATE_LEN;
    }

    if (s_ui.datetime_edit_target == UI_DATETIME_EDIT_TIME) {
        return STORAGE_SYSTEM_TIME_LEN;
    }

    return 0U;
}

static bool ui_service_datetime_edit_pos_editable(size_t pos)
{
    size_t len = ui_service_datetime_edit_len();
    char ch;

    if (pos >= len) {
        return false;
    }

    ch = s_ui.datetime_edit_buf[pos];
    return ch >= '0' && ch <= '9';
}

static void ui_service_move_datetime_cursor(int delta)
{
    size_t len = ui_service_datetime_edit_len();
    size_t pos = s_ui.datetime_edit_cursor;

    if (!ui_service_is_datetime_editing() || len == 0U) {
        return;
    }

    if (pos >= len) {
        pos = 0U;
    }

    if (delta < 0) {
        while (pos > 0U) {
            --pos;
            if (ui_service_datetime_edit_pos_editable(pos)) {
                s_ui.datetime_edit_cursor = pos;
                return;
            }
        }
    } else if (delta > 0) {
        while (pos + 1U < len) {
            ++pos;
            if (ui_service_datetime_edit_pos_editable(pos)) {
                s_ui.datetime_edit_cursor = pos;
                return;
            }
        }
    }
}

static void ui_service_format_datetime_edit_value(char *dest,
                                                  size_t dest_size,
                                                  const char *value,
                                                  size_t cursor)
{
    size_t out = 0U;

    if (dest == NULL || dest_size == 0U) {
        return;
    }

    dest[0] = '\0';
    if (value == NULL) {
        return;
    }

    for (size_t i = 0U; value[i] != '\0' && out + 1U < dest_size; ++i) {
        if (i == cursor && out + 3U < dest_size) {
            dest[out++] = '[';
            dest[out++] = value[i];
            dest[out++] = ']';
        } else {
            dest[out++] = value[i];
        }
    }

    dest[out] = '\0';
}

static void ui_service_begin_datetime_edit(uint8_t item, ui_datetime_edit_target_t target)
{
    platform_hal_datetime_t datetime;

    if (target == UI_DATETIME_EDIT_NONE) {
        return;
    }

    s_ui.datetime_edit_target = target;
    s_ui.edit_target = UI_EDIT_NONE;
    s_ui.text_edit_target = UI_TEXT_EDIT_NONE;
    s_ui.edit_item = item;

    if (platform_hal_get_datetime(&datetime) != ESP_OK) {
        if (target == UI_DATETIME_EDIT_DATE) {
            snprintf(s_ui.datetime_edit_buf, sizeof(s_ui.datetime_edit_buf), "%s", UI_SYSTEM_DEFAULT_DATE);
        } else {
            snprintf(s_ui.datetime_edit_buf, sizeof(s_ui.datetime_edit_buf), "%s", UI_SYSTEM_DEFAULT_TIME);
        }
    } else if (target == UI_DATETIME_EDIT_DATE) {
        ui_service_format_datetime_date(&datetime,
                                        s_ui.datetime_edit_buf,
                                        sizeof(s_ui.datetime_edit_buf));
    } else {
        ui_service_format_datetime_time(&datetime,
                                        s_ui.datetime_edit_buf,
                                        sizeof(s_ui.datetime_edit_buf));
    }

    s_ui.datetime_edit_cursor = 0U;
    ESP_LOGI(TAG, "date/time edit started: item=%u", (unsigned)item);
}

static bool ui_service_commit_datetime_edit(ui_input_event_t *out_event, char key)
{
    ui_setting_target_t setting;

    if (!ui_service_is_datetime_editing()) {
        return false;
    }

    setting = s_ui.datetime_edit_target == UI_DATETIME_EDIT_DATE ? UI_SETTING_SYSTEM_DATE
                                                                 : UI_SETTING_SYSTEM_TIME;
    ui_service_set_event(out_event, UI_INPUT_EVENT_DATETIME_CHANGED, key);
    if (out_event != NULL) {
        out_event->setting = setting;
        snprintf(out_event->text, sizeof(out_event->text), "%s", s_ui.datetime_edit_buf);
    }

    ESP_LOGI(TAG, "date/time edit committed");
    ui_service_clear_edit();
    return true;
}

static bool ui_service_handle_datetime_edit_char(char key, ui_input_event_t *out_event)
{
    if (!ui_service_is_datetime_editing()) {
        return false;
    }

    if (key == '\n' || key == '\r') {
        return ui_service_commit_datetime_edit(out_event, key);
    }

    if (key == '`' || key == '\x1B') {
        ui_service_clear_edit();
        return true;
    }

    if (key == ',') {
        ui_service_move_datetime_cursor(-1);
        return true;
    }

    if (key == '/') {
        ui_service_move_datetime_cursor(1);
        return true;
    }

    if (key == ';' || key == '.') {
        return true;
    }

    if (key >= '0' && key <= '9' &&
        ui_service_datetime_edit_pos_editable(s_ui.datetime_edit_cursor)) {
        s_ui.datetime_edit_buf[s_ui.datetime_edit_cursor] = key;
        ui_service_move_datetime_cursor(1);
        return true;
    }

    return true;
}

static void ui_service_set_event(ui_input_event_t *out_event,
                                 ui_input_event_type_t type,
                                 char key)
{
    if (out_event == NULL) {
        return;
    }

    out_event->type = type;
    out_event->key = key;
    out_event->setting = UI_SETTING_NONE;
    out_event->value = 0;
    out_event->delta = 0;
    out_event->text[0] = '\0';
}

static void ui_service_format_value_line(char *dest,
                                         size_t dest_size,
                                         uint8_t item,
                                         const char *prefix,
                                         int value,
                                         const char *suffix)
{
    if (ui_service_is_editing_item(item)) {
        if (s_ui.edit_buf[0] == '\0') {
            snprintf(dest, dest_size, "%s_%s", prefix, suffix ? suffix : "");
        } else {
            snprintf(dest, dest_size, "%s%s%s_", prefix, s_ui.edit_buf, suffix ? suffix : "");
        }
    } else {
        snprintf(dest, dest_size, "%s%d%s", prefix, value, suffix ? suffix : "");
    }
}

static void ui_service_copy_tail(char *dest, size_t dest_size, const char *text, size_t text_len)
{
    size_t start = 0;

    if (dest == NULL || dest_size == 0U) {
        return;
    }

    if (text == NULL) {
        dest[0] = '\0';
        return;
    }

    if (text_len >= dest_size) {
        start = text_len - (dest_size - 1U);
    }

    snprintf(dest, dest_size, "%s", &text[start]);
}

static void ui_service_format_text_edit_line(char *dest, size_t dest_size)
{
    const char *text = s_ui.text_edit_buf;
    size_t len;
    size_t visible_cols;
    size_t text_cols;
    size_t start = 0U;
    size_t marker_pos;
    size_t out = 0U;
    size_t src;

    if (dest == NULL || dest_size == 0U) {
        return;
    }

    dest[0] = '\0';
    if (dest_size <= 1U) {
        return;
    }

    ui_service_clamp_text_cursor();
    len = ui_service_text_edit_len();
    visible_cols = dest_size - 1U;
    if (visible_cols > UI_COLS) {
        visible_cols = UI_COLS;
    }

    if (visible_cols == 1U) {
        dest[0] = '_';
        dest[1] = '\0';
        return;
    }

    text_cols = visible_cols - 1U;
    if (len > text_cols) {
        if (s_ui.text_edit_cursor > text_cols / 2U) {
            start = s_ui.text_edit_cursor - (text_cols / 2U);
        }
        if (start + text_cols > len) {
            start = len - text_cols;
        }
    }

    marker_pos = s_ui.text_edit_cursor - start;
    if (marker_pos > text_cols) {
        marker_pos = text_cols;
    }

    for (src = start; out < marker_pos && src < len; ++src) {
        dest[out++] = text[src];
    }
    dest[out++] = '_';
    for (src = start + marker_pos; out < visible_cols && src < len; ++src) {
        dest[out++] = text[src];
    }
    dest[out] = '\0';
}

static void ui_service_format_accuracy_tenths(char *dest, size_t dest_size, uint16_t tenths)
{
    if (dest == NULL || dest_size == 0U) {
        return;
    }

    snprintf(dest, dest_size, "%u.%u%%", (unsigned)(tenths / 10U), (unsigned)(tenths % 10U));
}

static void ui_service_set_bottom_status(mini_cw_screen_t *screen)
{
    char status[32];

    if (screen == NULL) {
        return;
    }

    snprintf(status,
             sizeof(status),
             "TX:%u T:%uHz V:%u",
             (unsigned)keyer_service_get_key_in_wpm(),
             (unsigned)audio_service_get_tone_hz(),
             (unsigned)audio_service_get_volume());
    ui_service_set_text(screen->line[UI_MODE_LINES - 1U],
                        sizeof(screen->line[UI_MODE_LINES - 1U]),
                        status);
    screen->line_color[UI_MODE_LINES - 1U] = MINI_CW_SCREEN_COLOR_WHITE;
}

static void ui_service_render_lesson_normal(mini_cw_screen_t *screen)
{
    const cw_lesson_view_t *view = cw_trainer_lesson_get_view();
    char copy_tail[17];
    char active_preview[15];

    if (screen == NULL || view == NULL) {
        return;
    }

    switch (view->state) {
    case CW_LESSON_STATE_COPYING:
        ui_service_copy_tail(copy_tail, sizeof(copy_tail), view->copy_text, view->copy_len);
        snprintf(screen->line[0],
                 sizeof(screen->line[0]),
                 "L%02u Copy %u/%u",
                 view->config.lesson,
                 view->config.code_wpm,
                 view->config.effective_wpm);
        snprintf(screen->line[1], sizeof(screen->line[1]), "Typed:%u", view->copy_len);
        snprintf(screen->line[2], sizeof(screen->line[2]), "%s", copy_tail);
        ui_service_set_text(screen->line[3], sizeof(screen->line[3]), "Enter=check");
        ui_service_set_text(screen->line[4], sizeof(screen->line[4]), "` stop");
        ui_service_set_text(screen->line[5], sizeof(screen->line[5]), "Ctrl settings");
        break;
    case CW_LESSON_STATE_RESULT:
    {
        unsigned attempts = view->result.attempts > 9999U ? 9999U : view->result.attempts;
        snprintf(screen->line[0],
                 sizeof(screen->line[0]),
                 "Acc:%u Err:%u",
                 view->result.accuracy,
                 view->result.errors);
        snprintf(screen->line[1],
                 sizeof(screen->line[1]),
                 "S:%u C:%u",
                 view->result.target_chars,
                 view->result.copy_chars);
        snprintf(screen->line[2],
                 sizeof(screen->line[2]),
                 "B:%u T:%u",
                 view->result.best_accuracy,
                 attempts);
        ui_service_set_text(screen->line[3], sizeof(screen->line[3]), "Enter=new run");
        ui_service_set_text(screen->line[4], sizeof(screen->line[4]), "Ctrl settings");
        break;
    }
    case CW_LESSON_STATE_IDLE:
    case CW_LESSON_STATE_READY:
    default:
        memcpy(active_preview, view->active_chars, sizeof(active_preview) - 1U);
        active_preview[sizeof(active_preview) - 1U] = '\0';
        snprintf(screen->line[0],
                 sizeof(screen->line[0]),
                 "L%02u %u/%uW %umin",
                 view->config.lesson,
                 view->config.code_wpm,
                 view->config.effective_wpm,
                 view->config.duration_min);
        snprintf(screen->line[1], sizeof(screen->line[1]), "Chars:%s", active_preview);
        snprintf(screen->line[2], sizeof(screen->line[2]), "New:%c", view->new_char);
        snprintf(screen->line[3],
                 sizeof(screen->line[3]),
                 "Last:%u Best:%u",
                 view->result.last_accuracy,
                 view->result.best_accuracy);
        ui_service_set_text(screen->line[4], sizeof(screen->line[4]), "Enter=start");
        ui_service_set_text(screen->line[5], sizeof(screen->line[5]), "Ctrl settings");
        break;
    }
}

static void ui_service_render_word_normal(mini_cw_screen_t *screen)
{
    const cw_word_view_t *view = cw_trainer_word_get_view();
    char copy_tail[17];
    char last_tail[13];
    unsigned attempts;

    if (screen == NULL || view == NULL) {
        return;
    }

    switch (view->state) {
    case CW_WORD_STATE_COPYING:
        ui_service_copy_tail(copy_tail, sizeof(copy_tail), view->copy_text, view->copy_len);
        snprintf(screen->line[0],
                 sizeof(screen->line[0]),
                 "Word %02u/%02u %uw",
                 (unsigned)(view->current_index + 1U),
                 (unsigned)view->result.total_words,
                 (unsigned)view->current_wpm);
        snprintf(screen->line[1],
                 sizeof(screen->line[1]),
                 "Score:%lu Max:%u",
                 (unsigned long)view->result.score,
                 (unsigned)view->result.max_wpm);
        snprintf(screen->line[2], sizeof(screen->line[2]), "Typed:%.14s", copy_tail);
        if (view->last_sent_word != NULL && view->last_sent_word[0] != '\0') {
            ui_service_copy_tail(last_tail,
                                 sizeof(last_tail),
                                 view->last_sent_word,
                                 strlen(view->last_sent_word));
            snprintf(screen->line[3],
                     sizeof(screen->line[3]),
                     "%s:%s",
                     view->last_correct ? "OK" : "NO",
                     last_tail);
        } else {
            ui_service_set_text(screen->line[3], sizeof(screen->line[3]), "Last:-");
        }
        ui_service_set_text(screen->line[4], sizeof(screen->line[4]), "Enter=check .=play");
        ui_service_set_text(screen->line[5], sizeof(screen->line[5]), "` stop Ctrl menu");
        break;
    case CW_WORD_STATE_RESULT:
        attempts = view->result.attempts > 9999U ? 9999U : view->result.attempts;
        snprintf(screen->line[0],
                 sizeof(screen->line[0]),
                 "Done S:%lu",
                 (unsigned long)view->result.score);
        snprintf(screen->line[1],
                 sizeof(screen->line[1]),
                 "Max:%u OK:%u/%u",
                 (unsigned)view->result.max_wpm,
                 (unsigned)view->result.correct_count,
                 (unsigned)view->result.total_words);
        snprintf(screen->line[2],
                 sizeof(screen->line[2]),
                 "Best:%lu M:%u",
                 (unsigned long)view->result.best_score,
                 (unsigned)view->result.best_max_wpm);
        snprintf(screen->line[3], sizeof(screen->line[3]), "Attempts:%u", attempts);
        ui_service_set_text(screen->line[4], sizeof(screen->line[4]), "Enter=new run");
        ui_service_set_text(screen->line[5], sizeof(screen->line[5]), "Ctrl settings");
        break;
    case CW_WORD_STATE_IDLE:
    case CW_WORD_STATE_READY:
    default:
        snprintf(screen->line[0],
                 sizeof(screen->line[0]),
                 "Speed:%u Min:%u",
                 (unsigned)view->config.start_wpm,
                 (unsigned)view->config.min_char_wpm);
        snprintf(screen->line[1],
                 sizeof(screen->line[1]),
                 "L%02u MaxLen:%u",
                 (unsigned)view->config.lesson,
                 (unsigned)view->config.max_word_len);
        snprintf(screen->line[2],
                 sizeof(screen->line[2]),
                 "Last S:%lu M:%u",
                 (unsigned long)view->result.score,
                 (unsigned)view->result.max_wpm);
        snprintf(screen->line[3],
                 sizeof(screen->line[3]),
                 "Best:%lu M:%u",
                 (unsigned long)view->result.best_score,
                 (unsigned)view->result.best_max_wpm);
        ui_service_set_text(screen->line[4], sizeof(screen->line[4]), "Enter=start");
        ui_service_set_text(screen->line[5], sizeof(screen->line[5]), "Ctrl settings");
        break;
    }
}

static void ui_service_render_callsign_normal(mini_cw_screen_t *screen)
{
    const cw_callsign_view_t *view = cw_trainer_callsign_get_view();
    char copy_tail[17];
    char last_tail[13];
    unsigned attempts;

    if (screen == NULL || view == NULL) {
        return;
    }

    switch (view->state) {
    case CW_CALLSIGN_STATE_COPYING:
        ui_service_copy_tail(copy_tail, sizeof(copy_tail), view->copy_text, view->copy_len);
        snprintf(screen->line[0],
                 sizeof(screen->line[0]),
                 "Call %02u/%02u %uw",
                 (unsigned)(view->current_index + 1U),
                 (unsigned)view->result.total_calls,
                 (unsigned)view->current_wpm);
        snprintf(screen->line[1],
                 sizeof(screen->line[1]),
                 "Score:%lu Max:%u",
                 (unsigned long)view->result.score,
                 (unsigned)view->result.max_wpm);
        snprintf(screen->line[2], sizeof(screen->line[2]), "Typed:%.14s", copy_tail);
        if (view->last_sent_call != NULL && view->last_sent_call[0] != '\0') {
            ui_service_copy_tail(last_tail,
                                 sizeof(last_tail),
                                 view->last_sent_call,
                                 strlen(view->last_sent_call));
            snprintf(screen->line[3],
                     sizeof(screen->line[3]),
                     "%s:%s",
                     view->last_correct ? "OK" : "NO",
                     last_tail);
        } else {
            ui_service_set_text(screen->line[3], sizeof(screen->line[3]), "Last:-");
        }
        ui_service_set_text(screen->line[4], sizeof(screen->line[4]), "Enter=check ./sp");
        ui_service_set_text(screen->line[5], sizeof(screen->line[5]), "` stop Ctrl menu");
        break;
    case CW_CALLSIGN_STATE_RESULT:
        attempts = view->result.attempts > 9999U ? 9999U : view->result.attempts;
        snprintf(screen->line[0],
                 sizeof(screen->line[0]),
                 "Done S:%lu",
                 (unsigned long)view->result.score);
        snprintf(screen->line[1],
                 sizeof(screen->line[1]),
                 "Max:%u OK:%u/%u",
                 (unsigned)view->result.max_wpm,
                 (unsigned)view->result.correct_count,
                 (unsigned)view->result.total_calls);
        snprintf(screen->line[2],
                 sizeof(screen->line[2]),
                 "Best:%lu M:%u",
                 (unsigned long)view->result.best_score,
                 (unsigned)view->result.best_max_wpm);
        snprintf(screen->line[3], sizeof(screen->line[3]), "Attempts:%u", attempts);
        ui_service_set_text(screen->line[4], sizeof(screen->line[4]), "Enter=new run");
        ui_service_set_text(screen->line[5], sizeof(screen->line[5]), "Ctrl settings");
        break;
    case CW_CALLSIGN_STATE_IDLE:
    case CW_CALLSIGN_STATE_READY:
    default:
        snprintf(screen->line[0],
                 sizeof(screen->line[0]),
                 "Speed:%u Min:%u",
                 (unsigned)view->config.start_wpm,
                 (unsigned)view->config.min_char_wpm);
        snprintf(screen->line[1],
                 sizeof(screen->line[1]),
                 "MaxWPM:%u",
                 (unsigned)view->config.max_wpm);
        snprintf(screen->line[2],
                 sizeof(screen->line[2]),
                 "Last S:%lu M:%u",
                 (unsigned long)view->result.score,
                 (unsigned)view->result.max_wpm);
        snprintf(screen->line[3],
                 sizeof(screen->line[3]),
                 "Best:%lu M:%u",
                 (unsigned long)view->result.best_score,
                 (unsigned)view->result.best_max_wpm);
        ui_service_set_text(screen->line[4], sizeof(screen->line[4]), "Enter=start");
        ui_service_set_text(screen->line[5], sizeof(screen->line[5]), "Ctrl settings");
        break;
    }
}

static void ui_service_render_plaintext_normal(mini_cw_screen_t *screen)
{
    const cw_plaintext_view_t *view = cw_trainer_plaintext_get_view();
    char copy_tail[17];
    char title_tail[17];
    char acc[8];
    char best[8];
    char error_text[4];
    unsigned attempts;
    unsigned bounded_errors;

    if (screen == NULL || view == NULL) {
        return;
    }

    switch (view->state) {
    case CW_PLAINTEXT_STATE_COPYING:
        ui_service_copy_tail(copy_tail, sizeof(copy_tail), view->copy_text, view->copy_len);
        snprintf(screen->line[0],
                 sizeof(screen->line[0]),
                 "Plain Copy %u/%u",
                 (unsigned)view->config.code_wpm,
                 (unsigned)view->config.effective_wpm);
        snprintf(screen->line[1],
                 sizeof(screen->line[1]),
                 "Typed:%u/%u",
                 (unsigned)view->copy_len,
                 (unsigned)view->target_len);
        snprintf(screen->line[2], sizeof(screen->line[2]), "Copy:%.14s", copy_tail);
        ui_service_set_text(screen->line[3], sizeof(screen->line[3]), "Enter=check");
        ui_service_set_text(screen->line[4], sizeof(screen->line[4]), "` stop");
        ui_service_set_text(screen->line[5], sizeof(screen->line[5]), "Ctrl settings");
        break;
    case CW_PLAINTEXT_STATE_RESULT:
        attempts = view->result.attempts > 9999U ? 9999U : view->result.attempts;
        ui_service_format_accuracy_tenths(acc,
                                          sizeof(acc),
                                          view->result.accuracy_tenths);
        ui_service_format_accuracy_tenths(best,
                                          sizeof(best),
                                          view->result.best_accuracy_tenths);
        bounded_errors = view->result.errors > 999U ? 999U : view->result.errors;
        snprintf(error_text, sizeof(error_text), "%u", bounded_errors);
        snprintf(screen->line[0], sizeof(screen->line[0]), "Acc:%.6s E:%.3s", acc, error_text);
        snprintf(screen->line[1],
                 sizeof(screen->line[1]),
                 "Copy:%u/%u",
                 (unsigned)view->result.copy_chars,
                 (unsigned)view->result.target_chars);
        snprintf(screen->line[2], sizeof(screen->line[2]), "Best:%s", best);
        snprintf(screen->line[3], sizeof(screen->line[3]), "Attempts:%u", attempts);
        ui_service_set_text(screen->line[4], sizeof(screen->line[4]), "Enter=new text");
        ui_service_set_text(screen->line[5], sizeof(screen->line[5]), "Ctrl settings");
        break;
    case CW_PLAINTEXT_STATE_IDLE:
    case CW_PLAINTEXT_STATE_READY:
    default:
        ui_service_copy_tail(title_tail,
                             sizeof(title_tail),
                             view->title,
                             view->title != NULL ? strlen(view->title) : 0U);
        snprintf(screen->line[0],
                 sizeof(screen->line[0]),
                 "Plain %u/%uW",
                 (unsigned)view->config.code_wpm,
                 (unsigned)view->config.effective_wpm);
        snprintf(screen->line[1],
                 sizeof(screen->line[1]),
                 "Text:%.15s",
                 title_tail[0] != '\0' ? title_tail : "Built-in");
        if (view->result.attempts > 0U) {
            ui_service_format_accuracy_tenths(acc,
                                              sizeof(acc),
                                              view->result.last_accuracy_tenths);
            snprintf(screen->line[2], sizeof(screen->line[2]), "Last:%s", acc);
        } else {
            ui_service_set_text(screen->line[2], sizeof(screen->line[2]), "Last:-");
        }
        ui_service_format_accuracy_tenths(best,
                                          sizeof(best),
                                          view->result.best_accuracy_tenths);
        snprintf(screen->line[3], sizeof(screen->line[3]), "Best:%s", best);
        ui_service_set_text(screen->line[4], sizeof(screen->line[4]), "Enter=start");
        ui_service_set_text(screen->line[5], sizeof(screen->line[5]), "Ctrl settings");
        break;
    }
}

static void ui_service_render_keyer_normal(mini_cw_screen_t *screen)
{
    uint8_t row;
    char top[UI_COLS + 1];
    char tune_line[UI_COLS + 1];
    const char *line6 = s_keyer_tx_text;
    keyer_key_in_mode_t key_in_mode = keyer_service_get_key_in_mode();
    const char *key_in = keyer_service_key_in_mode_label(key_in_mode);
    const char *key_out = keyer_service_key_out_mode_label(keyer_service_get_key_out_mode());
    const char *op_name = keyer_service_get_op_name();
    uint8_t wpm = (key_in_mode == KEYER_KEY_IN_SK_T || key_in_mode == KEYER_KEY_IN_SK_R)
                      ? keyer_service_get_sk_wpm()
                      : keyer_service_get_key_in_wpm();

    if (screen == NULL) {
        return;
    }

    if (wpm > 99U) {
        wpm = 99U;
    }

    if (!s_ui.keyer_tune_active && op_name != NULL && op_name[0] != '\0') {
        snprintf(top, sizeof(top), "Keyer %-11.11s %2u", op_name, (unsigned)wpm);
    } else if (s_ui.keyer_tune_active) {
        snprintf(top, sizeof(top), "Tune  %5.5s %5.5s %2u", key_in, key_out, (unsigned)wpm);
    } else {
        snprintf(top, sizeof(top), "Keyer %5.5s %5.5s %2u", key_in, key_out, (unsigned)wpm);
    }
    ui_service_set_top_chars(screen, top, MINI_CW_SCREEN_COLOR_WHITE);

    if (!s_ui.keyer_tune_active && s_ui.keyer_shortcut_active) {
        for (row = 0U; row < UI_KEYER_VISIBLE_LINES; ++row) {
            snprintf(screen->line[row],
                     sizeof(screen->line[row]),
                     "M%u:%.17s",
                     (unsigned)(row + 1U),
                     keyer_service_get_message(row));
            screen->line_color[row] = MINI_CW_SCREEN_COLOR_GREEN;
        }
    } else {
        for (row = 0U; row < UI_KEYER_VISIBLE_LINES; ++row) {
            ui_service_keyer_render_history_line(screen->line[row],
                                                 sizeof(screen->line[row]),
                                                 (uint16_t)(s_keyer_history_scroll_top + row));
            screen->line_color[row] = MINI_CW_SCREEN_COLOR_GREEN;
        }
    }

    if (s_ui.keyer_tune_active) {
        if (keyer_service_get_tune_latched()) {
            ui_service_set_text(tune_line, sizeof(tune_line), "Tune:T");
        } else if (keyer_service_get_tune_output_active()) {
            ui_service_set_text(tune_line, sizeof(tune_line), "Tune:Hold");
        } else {
            ui_service_set_text(tune_line, sizeof(tune_line), "Tune");
        }
        line6 = tune_line;
    } else if (s_keyer_status_text[0] != '\0') {
        if (!ui_service_tick_reached(xTaskGetTickCount(), s_keyer_status_until_tick)) {
            line6 = s_keyer_status_text;
        } else {
            s_keyer_status_text[0] = '\0';
        }
    }

    ui_service_copy_tail(screen->line[5], sizeof(screen->line[5]), line6, strlen(line6));
    screen->line_color[5] = MINI_CW_SCREEN_COLOR_CYAN;
}

static void ui_service_render_system_normal(mini_cw_screen_t *screen)
{
    if (screen == NULL) {
        return;
    }

    ui_service_set_text(screen->line[0], sizeof(screen->line[0]), "Mini-CW V1.1");
    ui_service_set_text(screen->line[5], sizeof(screen->line[5]), "Ctrl->Change Setting");
}

static void ui_service_render_normal(void)
{
    mini_cw_screen_t screen;

    ui_service_prepare_screen(&screen);
    switch (s_ui.mode) {
    case UI_SERVICE_MODE_LESSONS:
        ui_service_render_lesson_normal(&screen);
        break;
    case UI_SERVICE_MODE_WORDS:
        ui_service_render_word_normal(&screen);
        break;
    case UI_SERVICE_MODE_CALLSIGNS:
        ui_service_render_callsign_normal(&screen);
        break;
    case UI_SERVICE_MODE_PLAINTEXT:
        ui_service_render_plaintext_normal(&screen);
        break;
    case UI_SERVICE_MODE_SYSTEM:
        ui_service_render_system_normal(&screen);
        break;
    case UI_SERVICE_MODE_KEYER:
    default:
        ui_service_render_keyer_normal(&screen);
        break;
    }
    if (s_ui.mode != UI_SERVICE_MODE_KEYER && s_ui.mode != UI_SERVICE_MODE_SYSTEM) {
        ui_service_set_bottom_status(&screen);
    }
    ui_screen_render(&screen);
}

static void ui_service_render_mode_select(void)
{
    mini_cw_screen_t screen;

    ui_service_prepare_screen(&screen);
    ui_service_set_text(screen.line[0], sizeof(screen.line[0]), "1 Keyer");
    ui_service_set_text(screen.line[1], sizeof(screen.line[1]), "2 Lessons");
    ui_service_set_text(screen.line[2], sizeof(screen.line[2]), "3 Words");
    ui_service_set_text(screen.line[3], sizeof(screen.line[3]), "4 Calls");
    ui_service_set_text(screen.line[4], sizeof(screen.line[4]), "5 Plain");
    ui_service_set_text(screen.line[5], sizeof(screen.line[5]), "6 System");

    ui_screen_render(&screen);
}

static void ui_service_render_no_settings(const char *label)
{
    mini_cw_screen_t screen;

    ui_service_prepare_screen(&screen);
    ui_service_set_text(screen.line[0], sizeof(screen.line[0]), label);

    ui_screen_render(&screen);
}

static void ui_service_render_lesson_menu(void)
{
    mini_cw_screen_t screen;
    const cw_lesson_config_t *config = cw_trainer_lesson_get_config();

    ui_service_prepare_screen(&screen);

    ui_service_format_value_line(screen.line[0],
                                 sizeof(screen.line[0]),
                                 1U,
                                 "1 Lesson:",
                                 config->lesson,
                                 "");
    ui_service_format_value_line(screen.line[1],
                                 sizeof(screen.line[1]),
                                 2U,
                                 "2 Duration:",
                                 config->duration_min,
                                 "m");
    ui_service_format_value_line(screen.line[2],
                                 sizeof(screen.line[2]),
                                 3U,
                                 "3 Code WPM:",
                                 config->code_wpm,
                                 "");
    ui_service_format_value_line(screen.line[3],
                                 sizeof(screen.line[3]),
                                 4U,
                                 "4 Eff WPM:",
                                 config->effective_wpm,
                                 "");

    if (ui_service_is_editing_item(5U)) {
        ui_service_format_value_line(screen.line[4],
                                     sizeof(screen.line[4]),
                                     5U,
                                     "5 Group:",
                                     config->group_len,
                                     "");
    } else if (config->group_len == 0U) {
        ui_service_set_text(screen.line[4], sizeof(screen.line[4]), "5 Group:Rand");
    } else {
        snprintf(screen.line[4], sizeof(screen.line[4]), "5 Group:%u", config->group_len);
    }

    ui_screen_render(&screen);
}

static void ui_service_render_keyer_menu(void)
{
    mini_cw_screen_t screen;

    ui_service_prepare_screen(&screen);

    if (s_ui.menu_page == 0U) {
        ui_service_format_value_line(screen.line[0],
                                     sizeof(screen.line[0]),
                                     1U,
                                     "1 Vol:",
                                     audio_service_get_volume(),
                                     "");
        snprintf(screen.line[1],
                 sizeof(screen.line[1]),
                 "2 Mute:%s",
                 keyer_service_get_mute() ? "ON" : "OFF");
        ui_service_format_value_line(screen.line[2],
                                     sizeof(screen.line[2]),
                                     3U,
                                     "3 Wpm:",
                                     keyer_service_get_key_in_wpm(),
                                     "");
        ui_service_format_value_line(screen.line[3],
                                     sizeof(screen.line[3]),
                                     4U,
                                     "4 Tone:",
                                     audio_service_get_tone_hz(),
                                     "Hz");
        snprintf(screen.line[4],
                 sizeof(screen.line[4]),
                 "5 keyIn:%s",
                 keyer_service_key_in_mode_label(keyer_service_get_key_in_mode()));
        snprintf(screen.line[5],
                 sizeof(screen.line[5]),
                 "6 keyOut:%s",
                 keyer_service_key_out_mode_label(keyer_service_get_key_out_mode()));
    } else if (s_ui.menu_page == 1U) {
        for (uint8_t i = 0U; i < KEYER_MESSAGE_COUNT; ++i) {
            const char *message = keyer_service_get_message(i);
            if (s_ui.text_edit_target == ui_service_message_text_target(i)) {
                ui_service_format_text_edit_line(screen.line[i], sizeof(screen.line[i]));
            } else {
                snprintf(screen.line[i],
                         sizeof(screen.line[i]),
                         "%u M%u:%.15s",
                         (unsigned)(i + 1U),
                         (unsigned)(i + 1U),
                         message);
            }
        }
        ui_service_format_value_line(screen.line[5],
                                     sizeof(screen.line[5]),
                                     6U,
                                     "6 RepeatInt:",
                                     keyer_service_get_repeat_interval_s(),
                                     "s");
    } else {
        snprintf(screen.line[0],
                 sizeof(screen.line[0]),
                 "1 Paddle:%s",
                 keyer_service_paddle_mode_label(keyer_service_get_paddle_mode()));
        ui_service_format_value_line(screen.line[1],
                                      sizeof(screen.line[1]),
                                      2U,
                                      "2 txDelay:",
                                      keyer_service_get_tx_delay_s(),
                                      "s");
        ui_service_format_value_line(screen.line[2],
                                     sizeof(screen.line[2]),
                                     3U,
                                     "3 TuneTimeout:",
                                     keyer_service_get_tune_timeout_s(),
                                     "s");
        if (s_ui.text_edit_target == UI_TEXT_EDIT_KEYER_MYCALL) {
            ui_service_format_text_edit_line(screen.line[3], sizeof(screen.line[3]));
        } else {
            snprintf(screen.line[3],
                     sizeof(screen.line[3]),
                     "4 myCall:%.11s",
                     keyer_service_get_mycall());
        }
        ui_service_format_value_line(screen.line[4],
                                     sizeof(screen.line[4]),
                                     5U,
                                     "5 SK Wpm:",
                                     keyer_service_get_sk_wpm(),
                                     "");
        ui_service_set_text(screen.line[5], sizeof(screen.line[5]), "");
    }

    ui_screen_render(&screen);
}

static void ui_service_render_word_menu(void)
{
    mini_cw_screen_t screen;
    const cw_word_config_t *config = cw_trainer_word_get_config();

    ui_service_prepare_screen(&screen);

    ui_service_format_value_line(screen.line[0],
                                 sizeof(screen.line[0]),
                                 1U,
                                 "1 Speed:",
                                 config->start_wpm,
                                 "");
    ui_service_format_value_line(screen.line[1],
                                 sizeof(screen.line[1]),
                                 2U,
                                 "2 MinChar:",
                                 config->min_char_wpm,
                                 "");
    ui_service_format_value_line(screen.line[2],
                                 sizeof(screen.line[2]),
                                 3U,
                                 "3 Lesson:",
                                 config->lesson,
                                 "");
    ui_service_format_value_line(screen.line[3],
                                 sizeof(screen.line[3]),
                                 4U,
                                 "4 MaxLen:",
                                 config->max_word_len,
                                 "");
    ui_service_format_value_line(screen.line[4],
                                 sizeof(screen.line[4]),
                                 5U,
                                 "5 MaxWPM:",
                                 config->max_wpm,
                                 "");
    ui_service_format_value_line(screen.line[5],
                                 sizeof(screen.line[5]),
                                 6U,
                                 "6 Delay_s:",
                                 config->delay_s,
                                 "");

    ui_screen_render(&screen);
}

static void ui_service_render_callsign_menu(void)
{
    mini_cw_screen_t screen;
    const cw_callsign_config_t *config = cw_trainer_callsign_get_config();

    ui_service_prepare_screen(&screen);

    ui_service_format_value_line(screen.line[0],
                                 sizeof(screen.line[0]),
                                 1U,
                                 "1 Speed:",
                                 config->start_wpm,
                                 "");
    ui_service_format_value_line(screen.line[1],
                                 sizeof(screen.line[1]),
                                 2U,
                                 "2 MinChar:",
                                 config->min_char_wpm,
                                 "");
    ui_service_format_value_line(screen.line[2],
                                 sizeof(screen.line[2]),
                                 3U,
                                 "3 MaxWPM:",
                                 config->max_wpm,
                                 "");
    ui_service_format_value_line(screen.line[3],
                                 sizeof(screen.line[3]),
                                 4U,
                                 "4 Delay_s:",
                                 config->delay_s,
                                 "");
    ui_service_set_text(screen.line[4], sizeof(screen.line[4]), "5");
    ui_service_set_text(screen.line[5], sizeof(screen.line[5]), "6");

    ui_screen_render(&screen);
}

static void ui_service_render_plaintext_menu(void)
{
    mini_cw_screen_t screen;
    const cw_plaintext_config_t *config = cw_trainer_plaintext_get_config();

    ui_service_prepare_screen(&screen);

    ui_service_format_value_line(screen.line[0],
                                 sizeof(screen.line[0]),
                                 1U,
                                 "1 Code WPM:",
                                 config->code_wpm,
                                 "");
    ui_service_format_value_line(screen.line[1],
                                 sizeof(screen.line[1]),
                                 2U,
                                 "2 Eff WPM:",
                                 config->effective_wpm,
                                 "");
    ui_service_set_text(screen.line[2], sizeof(screen.line[2]), "3");
    ui_service_set_text(screen.line[3], sizeof(screen.line[3]), "4");
    ui_service_set_text(screen.line[4], sizeof(screen.line[4]), "5");
    ui_service_set_text(screen.line[5], sizeof(screen.line[5]), "6");

    ui_screen_render(&screen);
}

static void ui_service_render_system_menu(void)
{
    mini_cw_screen_t screen;
    platform_hal_datetime_t datetime;
    platform_hal_datetime_t *datetime_ptr = &datetime;
    char date_value[STORAGE_SYSTEM_DATE_LEN + 3U];
    char time_value[STORAGE_SYSTEM_TIME_LEN + 3U];

    ui_service_prepare_screen(&screen);

    if (s_ui.menu_page == 0U) {
        ui_service_format_value_line(screen.line[0],
                                     sizeof(screen.line[0]),
                                     1U,
                                     "1 Volume:",
                                     audio_service_get_volume(),
                                     "");
        snprintf(screen.line[1],
                 sizeof(screen.line[1]),
                 "2 KeyIn:%s",
                 keyer_service_key_in_mode_label(keyer_service_get_key_in_mode()));
        ui_service_format_value_line(screen.line[2],
                                     sizeof(screen.line[2]),
                                     3U,
                                     "3 KeyIn WPM:",
                                     keyer_service_get_key_in_wpm(),
                                     "");
        snprintf(screen.line[3],
                 sizeof(screen.line[3]),
                 "4 Sleep/Batt %d%%",
                 ui_service_read_battery_percent());
        snprintf(screen.line[4],
                 sizeof(screen.line[4]),
                 "5 USB Drive:%s",
                 storage_usb_drive_is_enabled() ? "ON" : "OFF");
        ui_service_format_value_line(screen.line[5],
                                     sizeof(screen.line[5]),
                                     6U,
                                     "6 Tone:",
                                     audio_service_get_tone_hz(),
                                     "Hz");
    } else {
        if (platform_hal_get_datetime(&datetime) != ESP_OK) {
            datetime_ptr = NULL;
        }

        if (s_ui.datetime_edit_target == UI_DATETIME_EDIT_DATE) {
            ui_service_format_datetime_edit_value(date_value,
                                                  sizeof(date_value),
                                                  s_ui.datetime_edit_buf,
                                                  s_ui.datetime_edit_cursor);
        } else {
            ui_service_format_datetime_date(datetime_ptr, date_value, sizeof(date_value));
        }

        if (s_ui.datetime_edit_target == UI_DATETIME_EDIT_TIME) {
            ui_service_format_datetime_edit_value(time_value,
                                                  sizeof(time_value),
                                                  s_ui.datetime_edit_buf,
                                                  s_ui.datetime_edit_cursor);
            snprintf(screen.line[1], sizeof(screen.line[1]), "2 Time: %s", time_value);
        } else {
            ui_service_format_datetime_time(datetime_ptr, time_value, sizeof(time_value));
            snprintf(screen.line[1],
                     sizeof(screen.line[1]),
                     "2 Time: %s%s",
                     time_value,
                     datetime_ptr != NULL ? ui_service_time_source_suffix(datetime.source) : "");
        }

        snprintf(screen.line[0], sizeof(screen.line[0]), "1 Date: %s", date_value);
        ui_service_set_text(screen.line[2], sizeof(screen.line[2]), "3");
        ui_service_set_text(screen.line[3], sizeof(screen.line[3]), "4");
        ui_service_set_text(screen.line[4], sizeof(screen.line[4]), "5");
        ui_service_set_text(screen.line[5], sizeof(screen.line[5]), "6");
    }

    ui_screen_render(&screen);
}

static void ui_service_render_mode_menu(void)
{
    switch (s_ui.mode) {
    case UI_SERVICE_MODE_KEYER:
        ui_service_render_keyer_menu();
        break;
    case UI_SERVICE_MODE_LESSONS:
        ui_service_render_lesson_menu();
        break;
    case UI_SERVICE_MODE_WORDS:
        ui_service_render_word_menu();
        break;
    case UI_SERVICE_MODE_CALLSIGNS:
        ui_service_render_callsign_menu();
        break;
    case UI_SERVICE_MODE_PLAINTEXT:
        ui_service_render_plaintext_menu();
        break;
    case UI_SERVICE_MODE_SYSTEM:
        ui_service_render_system_menu();
        break;
    default:
        ui_service_render_no_settings("No settings");
        break;
    }
}

static void ui_service_render_current_view(void)
{
    switch (s_ui.view) {
    case UI_VIEW_MODE_SELECT:
        ui_service_render_mode_select();
        break;
    case UI_VIEW_MODE_MENU:
        ui_service_render_mode_menu();
        break;
    case UI_VIEW_NORMAL:
    default:
        ui_service_render_normal();
        break;
    }
}

static void ui_service_set_mode_internal(ui_service_mode_t mode)
{
    if (!ui_service_mode_is_valid(mode)) {
        mode = UI_SERVICE_MODE_KEYER;
    }

    s_ui.mode = mode;
    s_ui.menu_page = 0U;
    if (s_ui.mode != UI_SERVICE_MODE_KEYER) {
        s_ui.keyer_shortcut_active = false;
        s_ui.keyer_shortcut_macro = 0U;
        s_ui.keyer_tune_active = false;
    }
    ui_service_clear_edit();
    ESP_LOGI(TAG, "mode changed: %s", ui_service_mode_name(s_ui.mode));
}

static void ui_service_enter_mode_select(void)
{
    s_ui.view = UI_VIEW_MODE_SELECT;
    ui_service_clear_edit();
    ESP_LOGI(TAG, "mode selection entered");
    ui_service_render_current_view();
}

static void ui_service_exit_mode_select(void)
{
    if (s_ui.view == UI_VIEW_MODE_SELECT) {
        s_ui.view = UI_VIEW_NORMAL;
    }

    ui_service_clear_edit();
    ESP_LOGI(TAG, "mode selection exited");
    ui_service_render_current_view();
}

static void ui_service_toggle_mode_select(void)
{
    if (s_ui.view == UI_VIEW_MODE_SELECT) {
        ui_service_exit_mode_select();
    } else {
        ui_service_enter_mode_select();
    }
}

static void ui_service_enter_mode_menu(void)
{
    s_ui.view = UI_VIEW_MODE_MENU;
    s_ui.menu_page = 0U;
    ui_service_clear_edit();
    ESP_LOGI(TAG, "mode menu entered: %s", ui_service_mode_name(s_ui.mode));
    ui_service_render_current_view();
}

static void ui_service_exit_mode_menu(void)
{
    if (s_ui.view == UI_VIEW_MODE_MENU) {
        s_ui.view = UI_VIEW_NORMAL;
    }

    ui_service_clear_edit();
    ESP_LOGI(TAG, "mode menu exited");
    ui_service_render_current_view();
}

static void ui_service_toggle_mode_menu(void)
{
    if (s_ui.view == UI_VIEW_MODE_SELECT) {
        return;
    }

    if (s_ui.view == UI_VIEW_MODE_MENU) {
        ui_service_exit_mode_menu();
    } else {
        ui_service_enter_mode_menu();
    }
}

static bool ui_service_handle_mode_select_char(char key, ui_input_event_t *out_event)
{
    ui_service_mode_t mode;
    bool valid = true;

    switch (key) {
    case '1':
        mode = UI_SERVICE_MODE_KEYER;
        break;
    case '2':
        mode = UI_SERVICE_MODE_LESSONS;
        break;
    case '3':
        mode = UI_SERVICE_MODE_WORDS;
        break;
    case '4':
        mode = UI_SERVICE_MODE_CALLSIGNS;
        break;
    case '5':
        mode = UI_SERVICE_MODE_PLAINTEXT;
        break;
    case '6':
        mode = UI_SERVICE_MODE_SYSTEM;
        break;
    default:
        valid = false;
        break;
    }

    if (!valid) {
        return key >= '1' && key <= '6';
    }

    ui_service_set_mode_internal(mode);
    s_ui.view = UI_VIEW_NORMAL;
    ui_service_set_event(out_event, UI_INPUT_EVENT_MODE_CHANGED, key);
    return true;
}

static bool ui_service_menu_item_edit_target(uint8_t item, ui_edit_target_t *out_target)
{
    ui_edit_target_t target = UI_EDIT_NONE;

    if (s_ui.mode == UI_SERVICE_MODE_KEYER) {
        if (s_ui.menu_page == 0U) {
            if (item == 1U) {
                target = UI_EDIT_VOLUME;
            } else if (item == 3U) {
                target = UI_EDIT_KEY_IN_WPM;
            } else if (item == 4U) {
                target = UI_EDIT_TONE_HZ;
            }
        } else if (s_ui.menu_page == 1U) {
            if (item == 6U) {
                target = UI_EDIT_KEYER_REPEAT_INTERVAL_S;
            }
        } else if (s_ui.menu_page == 2U) {
            if (item == 2U) {
                target = UI_EDIT_KEYER_TX_DELAY_S;
            } else if (item == 3U) {
                target = UI_EDIT_KEYER_TUNE_TIMEOUT_S;
            } else if (item == 5U) {
                target = UI_EDIT_KEYER_SK_WPM;
            }
        }
    } else if (s_ui.mode == UI_SERVICE_MODE_SYSTEM && s_ui.menu_page == 0U) {
        if (item == 1U) {
            target = UI_EDIT_VOLUME;
        } else if (item == 3U) {
            target = UI_EDIT_KEY_IN_WPM;
        } else if (item == 6U) {
            target = UI_EDIT_TONE_HZ;
        }
    } else if (s_ui.mode == UI_SERVICE_MODE_LESSONS) {
        if (item == 1U) {
            target = UI_EDIT_LESSON;
        } else if (item == 2U) {
            target = UI_EDIT_LESSON_DURATION;
        } else if (item == 3U) {
            target = UI_EDIT_LESSON_CODE_WPM;
        } else if (item == 4U) {
            target = UI_EDIT_LESSON_EFFECTIVE_WPM;
        } else if (item == 5U) {
            target = UI_EDIT_LESSON_GROUP_LEN;
        }
    } else if (s_ui.mode == UI_SERVICE_MODE_WORDS) {
        if (item == 1U) {
            target = UI_EDIT_WORD_SPEED;
        } else if (item == 2U) {
            target = UI_EDIT_WORD_MIN_CHAR_WPM;
        } else if (item == 3U) {
            target = UI_EDIT_WORD_LESSON;
        } else if (item == 4U) {
            target = UI_EDIT_WORD_MAX_LEN;
        } else if (item == 5U) {
            target = UI_EDIT_WORD_MAX_WPM;
        } else if (item == 6U) {
            target = UI_EDIT_WORD_DELAY_S;
        }
    } else if (s_ui.mode == UI_SERVICE_MODE_CALLSIGNS) {
        if (item == 1U) {
            target = UI_EDIT_CALLSIGN_SPEED;
        } else if (item == 2U) {
            target = UI_EDIT_CALLSIGN_MIN_CHAR_WPM;
        } else if (item == 3U) {
            target = UI_EDIT_CALLSIGN_MAX_WPM;
        } else if (item == 4U) {
            target = UI_EDIT_CALLSIGN_DELAY_S;
        }
    } else if (s_ui.mode == UI_SERVICE_MODE_PLAINTEXT) {
        if (item == 1U) {
            target = UI_EDIT_PLAINTEXT_CODE_WPM;
        } else if (item == 2U) {
            target = UI_EDIT_PLAINTEXT_EFFECTIVE_WPM;
        }
    }

    if (out_target != NULL) {
        *out_target = target;
    }

    return target != UI_EDIT_NONE;
}

static bool ui_service_handle_edit_char(char key, ui_input_event_t *out_event)
{
    if (s_ui.edit_target == UI_EDIT_NONE) {
        return false;
    }

    if (key == '\n' || key == '\r') {
        (void)ui_service_commit_numeric_edit(out_event, key);
        return true;
    }

    if (key == '`' || key == '\x1B') {
        ESP_LOGI(TAG, "mode menu edit canceled");
        ui_service_clear_edit();
        return true;
    }

    if (key == '\b' || key == 0x7f) {
        ui_service_backspace_edit_digit();
        return true;
    }

    if (key == ',') {
        (void)ui_service_step_numeric_edit(-1, out_event, key);
        return true;
    }

    if (key == '/') {
        (void)ui_service_step_numeric_edit(1, out_event, key);
        return true;
    }

    if (key >= '0' && key <= '9') {
        ui_service_append_edit_digit(key);
        return true;
    }

    if (key == ';' || key == '.') {
        return true;
    }

    return false;
}

static bool ui_service_handle_menu_number(uint8_t item, char key, ui_input_event_t *out_event)
{
    ui_edit_target_t target;

    if (ui_service_menu_item_edit_target(item, &target)) {
        ui_service_begin_numeric_edit(item, target);
        return true;
    }

    if (s_ui.mode == UI_SERVICE_MODE_KEYER && s_ui.menu_page == 0U && item == 2U) {
        ui_service_set_event(out_event, UI_INPUT_EVENT_KEYER_MUTE_CHANGED, key);
        if (out_event != NULL) {
            out_event->setting = UI_SETTING_KEYER_MUTE;
            out_event->value = keyer_service_get_mute() ? 0 : 1;
            out_event->delta = keyer_service_get_mute() ? -1 : 1;
        }
        return true;
    }

    if (s_ui.mode == UI_SERVICE_MODE_KEYER && s_ui.menu_page == 0U && item == 5U) {
        ui_service_set_event(out_event, UI_INPUT_EVENT_KEY_IN_MODE_CHANGED, key);
        if (out_event != NULL) {
            out_event->setting = UI_SETTING_KEY_IN_MODE;
            out_event->delta = 1;
        }
        return true;
    }

    if (s_ui.mode == UI_SERVICE_MODE_KEYER && s_ui.menu_page == 0U && item == 6U) {
        ui_service_set_event(out_event, UI_INPUT_EVENT_KEY_OUT_MODE_CHANGED, key);
        if (out_event != NULL) {
            out_event->setting = UI_SETTING_KEY_OUT_MODE;
            out_event->delta = 1;
        }
        return true;
    }

    if (s_ui.mode == UI_SERVICE_MODE_KEYER && s_ui.menu_page == 1U && item >= 1U &&
        item <= KEYER_MESSAGE_COUNT) {
        ui_service_begin_text_edit(item, ui_service_message_text_target((uint8_t)(item - 1U)));
        return true;
    }

    if (s_ui.mode == UI_SERVICE_MODE_KEYER && s_ui.menu_page == 2U && item == 1U) {
        ui_service_set_event(out_event, UI_INPUT_EVENT_KEYER_PADDLE_MODE_CHANGED, key);
        if (out_event != NULL) {
            out_event->setting = UI_SETTING_KEYER_PADDLE_MODE;
            out_event->delta = 1;
        }
        return true;
    }

    if (s_ui.mode == UI_SERVICE_MODE_KEYER && s_ui.menu_page == 2U && item == 4U) {
        ui_service_begin_text_edit(item, UI_TEXT_EDIT_KEYER_MYCALL);
        return true;
    }

    if (s_ui.mode == UI_SERVICE_MODE_SYSTEM && s_ui.menu_page == 1U && item == 1U) {
        ui_service_begin_datetime_edit(item, UI_DATETIME_EDIT_DATE);
        return true;
    }

    if (s_ui.mode == UI_SERVICE_MODE_SYSTEM && s_ui.menu_page == 1U && item == 2U) {
        ui_service_begin_datetime_edit(item, UI_DATETIME_EDIT_TIME);
        return true;
    }

    if (s_ui.mode == UI_SERVICE_MODE_SYSTEM && s_ui.menu_page == 0U && item == 2U) {
        ui_service_set_event(out_event, UI_INPUT_EVENT_KEY_IN_MODE_CHANGED, key);
        if (out_event != NULL) {
            out_event->setting = UI_SETTING_KEY_IN_MODE;
            out_event->value = 0;
            out_event->delta = 1;
        }
        return true;
    }

    if (s_ui.mode == UI_SERVICE_MODE_SYSTEM && s_ui.menu_page == 0U && item == 4U) {
        ui_service_set_event(out_event, UI_INPUT_EVENT_SLEEP_REQUEST, key);
        ESP_LOGI(TAG, "sleep requested from system menu");
        return true;
    }

    if (s_ui.mode == UI_SERVICE_MODE_SYSTEM && s_ui.menu_page == 0U && item == 5U) {
        ui_service_set_event(out_event, UI_INPUT_EVENT_USB_DRIVE_CHANGED, key);
        if (out_event != NULL) {
            out_event->setting = UI_SETTING_USB_DRIVE;
            out_event->value = storage_usb_drive_is_enabled() ? 0 : 1;
            out_event->delta = storage_usb_drive_is_enabled() ? -1 : 1;
        }
        return true;
    }

    ESP_LOGI(TAG,
             "mode menu item %u selected in %s",
             (unsigned)item,
             ui_service_mode_name(s_ui.mode));
    return true;
}

static bool ui_service_handle_menu_char(char key, bool fn, ui_input_event_t *out_event)
{
    if (out_event != NULL) {
        *out_event = UI_EVENT_NONE;
    }

    if (ui_service_handle_datetime_edit_char(key, out_event)) {
        return true;
    }

    if (ui_service_handle_text_edit_char(key, out_event)) {
        return true;
    }

    if (ui_service_handle_edit_char(key, out_event)) {
        return true;
    }

    if (key >= '1' && key <= '6') {
        return ui_service_handle_menu_number((uint8_t)(key - '0'), key, out_event);
    }

    if (key == ';') {
        if (!fn && s_ui.menu_page > 0U) {
            --s_ui.menu_page;
        }
        return !fn;
    }

    if (key == '.') {
        if (!fn && s_ui.mode == UI_SERVICE_MODE_KEYER && s_ui.menu_page < 2U) {
            ++s_ui.menu_page;
        } else if (!fn && s_ui.mode == UI_SERVICE_MODE_SYSTEM && s_ui.menu_page < 1U) {
            ++s_ui.menu_page;
        }
        return !fn;
    }

    if (key == ',' || key == '/') {
        return true;
    }

    return false;
}

static ui_input_event_t ui_service_map_normal_char(char ch)
{
    ui_input_event_t event = UI_EVENT_NONE;

    event.key = ch;

    if (ch == '\n' || ch == '\r') {
        event.type = UI_INPUT_EVENT_SELECT;
    } else if (ch == '\b' || ch == 0x7f) {
        event.type = UI_INPUT_EVENT_BACKSPACE;
    } else if (ch == '`' || ch == '\x1B') {
        event.type = UI_INPUT_EVENT_CANCEL;
    } else if (ch == '+' || ch == '-' || ch == '_' || ch == '[' || ch == ']') {
        event.type = UI_INPUT_EVENT_NONE;
    } else if (s_ui.mode == UI_SERVICE_MODE_LESSONS && ch >= 32 && ch <= 126) {
        event.type = UI_INPUT_EVENT_CHAR_INPUT;
    } else if (s_ui.mode == UI_SERVICE_MODE_WORDS && ch == '.') {
        event.type = UI_INPUT_EVENT_REPLAY;
    } else if (s_ui.mode == UI_SERVICE_MODE_WORDS && ch >= 32 && ch <= 126) {
        event.type = UI_INPUT_EVENT_CHAR_INPUT;
    } else if (s_ui.mode == UI_SERVICE_MODE_CALLSIGNS && (ch == '.' || ch == ' ')) {
        event.type = UI_INPUT_EVENT_REPLAY;
    } else if (s_ui.mode == UI_SERVICE_MODE_CALLSIGNS && ch >= 32 && ch <= 126) {
        event.type = UI_INPUT_EVENT_CHAR_INPUT;
    } else if (s_ui.mode == UI_SERVICE_MODE_PLAINTEXT && ch >= 32 && ch <= 126) {
        event.type = UI_INPUT_EVENT_CHAR_INPUT;
    } else if (s_ui.mode == UI_SERVICE_MODE_KEYER &&
               ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') || ch == '.' || ch == ',' || ch == '?' ||
                ch == '/' || ch == '=' || ch == ' ')) {
        event.type = UI_INPUT_EVENT_CHAR_INPUT;
    }

    return event;
}

static bool ui_service_handle_keyer_fn_scroll(const ui_cardputer_port_event_t *port_event)
{
    if (port_event == NULL || port_event->type != UI_CARDPUTER_PORT_EVENT_CHAR) {
        return false;
    }
    if (s_ui.view != UI_VIEW_NORMAL || s_ui.mode != UI_SERVICE_MODE_KEYER || !port_event->fn) {
        return false;
    }

    if (port_event->ch == ';') {
        ui_service_keyer_scroll_decoded(-1);
        ui_service_render_current_view();
        return true;
    }
    if (port_event->ch == '.') {
        ui_service_keyer_scroll_decoded(1);
        ui_service_render_current_view();
        return true;
    }

    return false;
}

static bool ui_service_is_text_cursor_key(char key)
{
    return key == ',' || key == '/';
}

static bool ui_service_handle_text_cursor_input(const ui_cardputer_port_event_t *port_event,
                                                bool port_event_ready)
{
    TickType_t now;
    int delta;

    if (!ui_service_is_text_editing()) {
        ui_service_reset_text_cursor_repeat();
        return false;
    }

    if (port_event == NULL || !port_event->fn || !ui_service_is_text_cursor_key(port_event->ch)) {
        ui_service_reset_text_cursor_repeat();
        return false;
    }

    now = xTaskGetTickCount();
    delta = port_event->ch == ',' ? -1 : 1;

    if (port_event_ready && port_event->type == UI_CARDPUTER_PORT_EVENT_CHAR) {
        ui_service_move_text_cursor(delta);
        s_ui.text_edit_cursor_repeat_active = true;
        s_ui.text_edit_cursor_repeat_key = port_event->ch;
        s_ui.text_edit_cursor_repeat_due =
            now + pdMS_TO_TICKS(UI_TEXT_CURSOR_REPEAT_DELAY_MS);
        ui_service_render_current_view();
        return true;
    }

    if (!s_ui.text_edit_cursor_repeat_active ||
        s_ui.text_edit_cursor_repeat_key != port_event->ch) {
        s_ui.text_edit_cursor_repeat_active = true;
        s_ui.text_edit_cursor_repeat_key = port_event->ch;
        s_ui.text_edit_cursor_repeat_due =
            now + pdMS_TO_TICKS(UI_TEXT_CURSOR_REPEAT_DELAY_MS);
        return true;
    }

    if (ui_service_tick_reached(now, s_ui.text_edit_cursor_repeat_due)) {
        ui_service_move_text_cursor(delta);
        s_ui.text_edit_cursor_repeat_due =
            now + pdMS_TO_TICKS(UI_TEXT_CURSOR_REPEAT_INTERVAL_MS);
        ui_service_render_current_view();
    }

    return true;
}

static bool ui_service_emit_keyer_wpm_delta(char key, ui_input_event_t *out_event)
{
    char status[UI_COLS + 1];
    int next;

    if (key != '[' && key != ']') {
        return false;
    }

    next = keyer_service_get_key_in_wpm() + (key == ']' ? 1 : -1);
    next = ui_service_clamp_int(next, UI_WPM_MIN, UI_WPM_MAX);
    ui_service_set_event(out_event, UI_INPUT_EVENT_KEY_IN_WPM_CHANGED, key);
    if (out_event != NULL) {
        out_event->setting = UI_SETTING_KEY_IN_WPM;
        out_event->value = next;
        out_event->delta = key == ']' ? 1 : -1;
    }
    snprintf(status, sizeof(status), "Wpm:%d", next);
    ui_service_keyer_set_status(status);
    return true;
}

static bool ui_service_emit_keyer_mute_toggle(char key, ui_input_event_t *out_event)
{
    bool next;

    if (key != '\\') {
        return false;
    }

    next = !keyer_service_get_mute();
    ui_service_keyer_set_status(next ? "Mute:ON" : "Mute:OFF");
    ui_service_set_event(out_event, UI_INPUT_EVENT_KEYER_MUTE_CHANGED, key);
    if (out_event != NULL) {
        out_event->setting = UI_SETTING_KEYER_MUTE;
        out_event->value = next ? 1 : 0;
        out_event->delta = next ? 1 : -1;
    }
    return true;
}

static bool ui_service_handle_keyer_tune_char(char key, ui_input_event_t *out_event)
{
    if (s_ui.mode != UI_SERVICE_MODE_KEYER || s_ui.view != UI_VIEW_NORMAL) {
        return false;
    }

    if (key == '\t') {
        s_ui.keyer_tune_active = !s_ui.keyer_tune_active;
        s_ui.keyer_shortcut_active = false;
        s_ui.keyer_shortcut_macro = 0U;
        s_keyer_status_text[0] = '\0';
        ui_service_clear_edit();
        ui_service_set_event(out_event, UI_INPUT_EVENT_KEYER_TUNE_CHANGED, key);
        if (out_event != NULL) {
            out_event->value = s_ui.keyer_tune_active ? 1 : 0;
        }
        return true;
    }

    if (!s_ui.keyer_tune_active) {
        return false;
    }

    if (key == 't' || key == 'T') {
        bool next = !keyer_service_get_tune_latched();
        ui_service_set_event(out_event, UI_INPUT_EVENT_KEYER_TUNE_LATCH_CHANGED, key);
        if (out_event != NULL) {
            out_event->value = next ? 1 : 0;
        }
        return true;
    }

    return true;
}

static bool ui_service_handle_keyer_shortcut_char(char key, ui_input_event_t *out_event)
{
    if (!s_ui.keyer_shortcut_active || s_ui.mode != UI_SERVICE_MODE_KEYER ||
        s_ui.view != UI_VIEW_NORMAL) {
        return false;
    }

    if (key >= '1' && key <= '5') {
        uint8_t index = (uint8_t)(key - '0');
        s_ui.keyer_shortcut_macro = index;
        ui_service_set_event(out_event, UI_INPUT_EVENT_KEYER_MACRO_SELECTED, key);
        if (out_event != NULL) {
            out_event->value = index;
        }
        return true;
    }

    return false;
}

void ui_service_init(void)
{
    s_cardputer_ready = ui_cardputer_port_init();
    ui_screen_init();
    ESP_LOGI(TAG,
             "display/keyboard owner: %s",
             s_cardputer_ready ? "M5Cardputer mic_test path" : "log fallback");
}

void ui_service_show_demo_screen(void)
{
    s_ui.view = UI_VIEW_NORMAL;
    ui_service_clear_edit();
    ui_service_render_current_view();
}

void ui_service_refresh(void)
{
    ui_service_render_current_view();
}

ui_service_mode_t ui_service_get_mode(void)
{
    return s_ui.mode;
}

void ui_service_set_mode(ui_service_mode_t mode)
{
    ui_service_set_mode_internal(mode);
    s_ui.view = UI_VIEW_NORMAL;
    ui_service_render_current_view();
}

void ui_service_prepare_for_sleep(void)
{
    ui_service_clear_edit();
    ui_cardputer_port_display_sleep();
}

ui_input_event_t ui_service_poll_input(void)
{
    ui_cardputer_port_event_t port_event;
    bool port_event_ready = ui_cardputer_port_poll_input(&port_event);

    if (ui_service_handle_text_cursor_input(&port_event, port_event_ready)) {
        return UI_EVENT_NONE;
    }

    if (!port_event_ready) {
        return UI_EVENT_NONE;
    }

    if (s_ui.mode == UI_SERVICE_MODE_KEYER && s_ui.view == UI_VIEW_NORMAL &&
        s_ui.keyer_tune_active && port_event.type != UI_CARDPUTER_PORT_EVENT_CHAR) {
        return UI_EVENT_NONE;
    }

    if (port_event.type == UI_CARDPUTER_PORT_EVENT_OPT) {
        ui_service_toggle_mode_select();
        return UI_EVENT_NONE;
    }

    if (port_event.type == UI_CARDPUTER_PORT_EVENT_CTRL) {
        ui_service_toggle_mode_menu();
        return UI_EVENT_NONE;
    }

    if (port_event.type == UI_CARDPUTER_PORT_EVENT_ALT) {
        ui_input_event_t shortcut_event = UI_EVENT_NONE;
        if (s_ui.view == UI_VIEW_NORMAL && s_ui.mode == UI_SERVICE_MODE_KEYER) {
            s_ui.keyer_shortcut_active = !s_ui.keyer_shortcut_active;
            s_ui.keyer_shortcut_macro = 0U;
            ui_service_clear_edit();
            ui_service_set_event(&shortcut_event, UI_INPUT_EVENT_KEYER_SHORTCUT_CHANGED, '\0');
            shortcut_event.value = s_ui.keyer_shortcut_active ? 1 : 0;
            ui_service_render_current_view();
            return shortcut_event;
        }
        return UI_EVENT_NONE;
    }

    if (port_event.type == UI_CARDPUTER_PORT_EVENT_BACKSPACE_HOLD) {
        ui_input_event_t clear_event = UI_EVENT_NONE;
        if (s_ui.view == UI_VIEW_NORMAL && s_ui.mode == UI_SERVICE_MODE_KEYER) {
            ui_service_set_event(&clear_event, UI_INPUT_EVENT_KEYER_CLEAR, '\b');
            return clear_event;
        }
        return UI_EVENT_NONE;
    }

    if (port_event.type == UI_CARDPUTER_PORT_EVENT_FN) {
        return UI_EVENT_NONE;
    }

    if (port_event.type != UI_CARDPUTER_PORT_EVENT_CHAR) {
        return UI_EVENT_NONE;
    }

    if (s_ui.mode == UI_SERVICE_MODE_KEYER && s_ui.view == UI_VIEW_NORMAL) {
        ui_input_event_t tune_event = UI_EVENT_NONE;
        if (ui_service_handle_keyer_tune_char(port_event.ch, &tune_event)) {
            return tune_event;
        }
    }

    if (ui_service_handle_keyer_fn_scroll(&port_event)) {
        return UI_EVENT_NONE;
    }

    if (s_ui.view == UI_VIEW_MODE_SELECT) {
        ui_input_event_t mode_event = UI_EVENT_NONE;

        if (ui_service_handle_mode_select_char(port_event.ch, &mode_event)) {
            ui_service_render_current_view();
        }

        if (mode_event.type != UI_INPUT_EVENT_NONE) {
            return mode_event;
        }

        return UI_EVENT_NONE;
    }

    if (s_ui.view == UI_VIEW_MODE_MENU) {
        ui_input_event_t menu_event = UI_EVENT_NONE;

        if (ui_service_handle_menu_char(port_event.ch, port_event.fn, &menu_event)) {
            if (menu_event.type == UI_INPUT_EVENT_NONE) {
                ui_service_render_current_view();
            }
        }

        if (menu_event.type != UI_INPUT_EVENT_NONE) {
            return menu_event;
        }

        return UI_EVENT_NONE;
    }

    if (s_ui.mode == UI_SERVICE_MODE_KEYER) {
        ui_input_event_t keyer_event = UI_EVENT_NONE;

        if (ui_service_emit_keyer_wpm_delta(port_event.ch, &keyer_event)) {
            return keyer_event;
        }

        if (ui_service_emit_keyer_mute_toggle(port_event.ch, &keyer_event)) {
            return keyer_event;
        }

        if (ui_service_handle_keyer_shortcut_char(port_event.ch, &keyer_event)) {
            if (keyer_event.type == UI_INPUT_EVENT_NONE) {
                ui_service_render_current_view();
            }
            return keyer_event;
        }
    }

    ui_input_event_t event = ui_service_map_normal_char(port_event.ch);

    if (event.type != UI_INPUT_EVENT_NONE) {
        ESP_LOGI(TAG, "input event: type=%d key='%c'", event.type, event.key);
    }

    return event;
}

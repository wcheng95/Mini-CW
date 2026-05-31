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
#include "keyer_service.h"
#include "platform_hal.h"

#include <stdbool.h>
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
    UI_EDIT_CALLSIGN_SPEED,
    UI_EDIT_CALLSIGN_MIN_CHAR_WPM,
    UI_EDIT_CALLSIGN_MAX_WPM,
} ui_edit_target_t;

typedef struct {
    ui_service_mode_t mode;
    ui_view_t view;
    uint8_t menu_page;
    ui_edit_target_t edit_target;
    uint8_t edit_item;
    char edit_buf[4];
    bool edit_user_digits;
} ui_service_state_t;

static const ui_input_event_t UI_EVENT_NONE = {
    .type = UI_INPUT_EVENT_NONE,
    .key = '\0',
    .setting = UI_SETTING_NONE,
    .value = 0,
    .delta = 0,
};

static ui_service_state_t s_ui = {
    .mode = UI_SERVICE_MODE_KEYER,
    .view = UI_VIEW_NORMAL,
    .menu_page = 0U,
    .edit_target = UI_EDIT_NONE,
    .edit_item = 0U,
    .edit_buf = "",
    .edit_user_digits = false,
};

static bool s_cardputer_ready;

#define UI_VOLUME_MIN 0
#define UI_VOLUME_MAX 99
#define UI_VOLUME_STEP 5
#define UI_WPM_MIN 5
#define UI_WPM_MAX 60
#define UI_WPM_STEP 1

static void ui_service_render_current_view(void);
static void ui_service_set_event(ui_input_event_t *out_event,
                                 ui_input_event_type_t type,
                                 char key);

static bool ui_service_mode_is_valid(ui_service_mode_t mode)
{
    return mode >= UI_SERVICE_MODE_PRACTICE && mode <= UI_SERVICE_MODE_SYSTEM;
}

static const char *ui_service_mode_name(ui_service_mode_t mode)
{
    switch (mode) {
    case UI_SERVICE_MODE_PRACTICE:
        return "Practice";
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

static void ui_service_copy_fixed_field(char *dest,
                                        size_t dest_size,
                                        size_t offset,
                                        size_t width,
                                        const char *text)
{
    size_t i = 0;

    if (dest == NULL || offset >= dest_size) {
        return;
    }

    for (; i < width && offset + i + 1U < dest_size && text != NULL && text[i] != '\0'; ++i) {
        dest[offset + i] = text[i];
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

static bool ui_service_is_editing_item(uint8_t item)
{
    return s_ui.edit_target != UI_EDIT_NONE && s_ui.edit_item == item;
}

static void ui_service_clear_edit(void)
{
    s_ui.edit_target = UI_EDIT_NONE;
    s_ui.edit_item = 0U;
    s_ui.edit_buf[0] = '\0';
    s_ui.edit_user_digits = false;
}

static void ui_service_prepare_screen(mini_cw_screen_t *screen)
{
    if (screen == NULL) {
        return;
    }

    memset(screen, 0, sizeof(*screen));
    ui_service_set_text(screen->mode, sizeof(screen->mode), ui_service_mode_name(s_ui.mode));
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
    case UI_EDIT_CALLSIGN_SPEED:
    case UI_EDIT_CALLSIGN_MIN_CHAR_WPM:
    case UI_EDIT_CALLSIGN_MAX_WPM:
        return UI_WPM_MIN;
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
    case UI_EDIT_KEY_IN_WPM:
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
    case UI_EDIT_CALLSIGN_SPEED:
    case UI_EDIT_CALLSIGN_MIN_CHAR_WPM:
    case UI_EDIT_CALLSIGN_MAX_WPM:
        return 40;
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
    case UI_EDIT_CALLSIGN_SPEED:
    case UI_EDIT_CALLSIGN_MIN_CHAR_WPM:
    case UI_EDIT_CALLSIGN_MAX_WPM:
        return 1;
    case UI_EDIT_NONE:
    default:
        return 1;
    }
}

static size_t ui_service_edit_max_digits(ui_edit_target_t target)
{
    if (target == UI_EDIT_LESSON_DURATION || target == UI_EDIT_LESSON_GROUP_LEN) {
        return 1U;
    }

    return 2U;
}

static int ui_service_get_edit_value(ui_edit_target_t target)
{
    switch (target) {
    case UI_EDIT_VOLUME:
        return audio_service_get_volume();
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
    case UI_EDIT_CALLSIGN_SPEED:
        return cw_trainer_callsign_get_config()->start_wpm;
    case UI_EDIT_CALLSIGN_MIN_CHAR_WPM:
        return cw_trainer_callsign_get_config()->min_char_wpm;
    case UI_EDIT_CALLSIGN_MAX_WPM:
        return cw_trainer_callsign_get_config()->max_wpm;
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
        return UI_INPUT_EVENT_WORD_CONFIG_CHANGED;
    case UI_EDIT_CALLSIGN_SPEED:
    case UI_EDIT_CALLSIGN_MIN_CHAR_WPM:
    case UI_EDIT_CALLSIGN_MAX_WPM:
        return UI_INPUT_EVENT_CALLSIGN_CONFIG_CHANGED;
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
    case UI_EDIT_CALLSIGN_SPEED:
        return UI_SETTING_CALLSIGN_SPEED;
    case UI_EDIT_CALLSIGN_MIN_CHAR_WPM:
        return UI_SETTING_CALLSIGN_MIN_CHAR_WPM;
    case UI_EDIT_CALLSIGN_MAX_WPM:
        return UI_SETTING_CALLSIGN_MAX_WPM;
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

static void ui_service_render_keyer_normal(mini_cw_screen_t *screen)
{
    unsigned wpm;
    char wpm_text[3];

    if (screen == NULL) {
        return;
    }

    memset(screen->line[5], ' ', UI_COLS);
    screen->line[5][UI_COLS] = '\0';

    ui_service_copy_fixed_field(screen->line[5],
                                sizeof(screen->line[5]),
                                0U,
                                8U,
                                keyer_service_io_mode_label(keyer_service_get_key_in_mode()));
    ui_service_copy_fixed_field(screen->line[5],
                                sizeof(screen->line[5]),
                                9U,
                                8U,
                                keyer_service_io_mode_label(keyer_service_get_key_out_mode()));

    wpm = keyer_service_get_key_in_wpm();
    if (wpm > 99U) {
        wpm = 99U;
    }
    snprintf(wpm_text, sizeof(wpm_text), "%2u", wpm);
    ui_service_copy_fixed_field(screen->line[5], sizeof(screen->line[5]), 18U, 2U, wpm_text);
}

static void ui_service_render_practice_normal(mini_cw_screen_t *screen)
{
    if (screen == NULL) {
        return;
    }

    ui_service_set_text(screen->line[0], sizeof(screen->line[0]), "Practice ready");
    ui_service_set_text(screen->line[5], sizeof(screen->line[5]), cw_trainer_get_status());
}

static void ui_service_render_system_normal(mini_cw_screen_t *screen)
{
    if (screen == NULL) {
        return;
    }

    ui_service_set_text(screen->line[0], sizeof(screen->line[0]), "System ready");
    ui_service_set_text(screen->line[5], sizeof(screen->line[5]), "Ctrl settings");
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
    case UI_SERVICE_MODE_PRACTICE:
        ui_service_render_practice_normal(&screen);
        break;
    case UI_SERVICE_MODE_SYSTEM:
        ui_service_render_system_normal(&screen);
        break;
    case UI_SERVICE_MODE_KEYER:
    default:
        ui_service_render_keyer_normal(&screen);
        break;
    }
    ui_screen_render(&screen);
}

static void ui_service_render_mode_select(void)
{
    mini_cw_screen_t screen;

    ui_service_prepare_screen(&screen);
    ui_service_set_text(screen.line[0], sizeof(screen.line[0]), "1 Practice");
    ui_service_set_text(screen.line[1], sizeof(screen.line[1]), "2 Keyer");
    ui_service_set_text(screen.line[2], sizeof(screen.line[2]), "3 Lessons");
    ui_service_set_text(screen.line[3], sizeof(screen.line[3]), "4 Words");
    ui_service_set_text(screen.line[4], sizeof(screen.line[4]), "5 Calls");
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
    ui_service_set_text(screen.line[4], sizeof(screen.line[4]), "5");
    ui_service_set_text(screen.line[5], sizeof(screen.line[5]), "6");

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
    ui_service_set_text(screen.line[3], sizeof(screen.line[3]), "4");
    ui_service_set_text(screen.line[4], sizeof(screen.line[4]), "5");
    ui_service_set_text(screen.line[5], sizeof(screen.line[5]), "6");

    ui_screen_render(&screen);
}

static void ui_service_render_system_menu(void)
{
    mini_cw_screen_t screen;

    ui_service_prepare_screen(&screen);

    ui_service_format_value_line(screen.line[0],
                                 sizeof(screen.line[0]),
                                 1U,
                                 "1 Volume:",
                                 audio_service_get_volume(),
                                 "");
    snprintf(screen.line[1],
             sizeof(screen.line[1]),
             "2 KeyIn:%s",
             keyer_service_io_mode_label(keyer_service_get_key_in_mode()));
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
    ui_service_set_text(screen.line[4], sizeof(screen.line[4]), "5 Date");
    ui_service_set_text(screen.line[5], sizeof(screen.line[5]), "6 Time");

    ui_screen_render(&screen);
}

static void ui_service_render_mode_menu(void)
{
    switch (s_ui.mode) {
    case UI_SERVICE_MODE_LESSONS:
        ui_service_render_lesson_menu();
        break;
    case UI_SERVICE_MODE_WORDS:
        ui_service_render_word_menu();
        break;
    case UI_SERVICE_MODE_CALLSIGNS:
        ui_service_render_callsign_menu();
        break;
    case UI_SERVICE_MODE_SYSTEM:
        ui_service_render_system_menu();
        break;
    case UI_SERVICE_MODE_PRACTICE:
        ui_service_render_no_settings("No settings");
        break;
    case UI_SERVICE_MODE_KEYER:
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
        mode = UI_SERVICE_MODE_PRACTICE;
        break;
    case '2':
        mode = UI_SERVICE_MODE_KEYER;
        break;
    case '3':
        mode = UI_SERVICE_MODE_LESSONS;
        break;
    case '4':
        mode = UI_SERVICE_MODE_WORDS;
        break;
    case '5':
        mode = UI_SERVICE_MODE_CALLSIGNS;
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

    if (s_ui.mode == UI_SERVICE_MODE_SYSTEM) {
        if (item == 1U) {
            target = UI_EDIT_VOLUME;
        } else if (item == 3U) {
            target = UI_EDIT_KEY_IN_WPM;
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
        }
    } else if (s_ui.mode == UI_SERVICE_MODE_CALLSIGNS) {
        if (item == 1U) {
            target = UI_EDIT_CALLSIGN_SPEED;
        } else if (item == 2U) {
            target = UI_EDIT_CALLSIGN_MIN_CHAR_WPM;
        } else if (item == 3U) {
            target = UI_EDIT_CALLSIGN_MAX_WPM;
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

    if (s_ui.mode == UI_SERVICE_MODE_SYSTEM && item == 2U) {
        ui_service_set_event(out_event, UI_INPUT_EVENT_KEY_IN_MODE_CHANGED, key);
        if (out_event != NULL) {
            out_event->setting = UI_SETTING_KEY_IN_MODE;
            out_event->value = 0;
            out_event->delta = 1;
        }
        return true;
    }

    if (s_ui.mode == UI_SERVICE_MODE_SYSTEM && item == 4U) {
        ui_service_set_event(out_event, UI_INPUT_EVENT_SLEEP_REQUEST, key);
        ESP_LOGI(TAG, "sleep requested from system menu");
        return true;
    }

    ESP_LOGI(TAG,
             "mode menu item %u selected in %s",
             (unsigned)item,
             ui_service_mode_name(s_ui.mode));
    return true;
}

static bool ui_service_handle_menu_char(char key, ui_input_event_t *out_event)
{
    if (out_event != NULL) {
        *out_event = UI_EVENT_NONE;
    }

    if (ui_service_handle_edit_char(key, out_event)) {
        return true;
    }

    if (key >= '1' && key <= '6') {
        return ui_service_handle_menu_number((uint8_t)(key - '0'), key, out_event);
    }

    if (key == ';') {
        if (s_ui.menu_page > 0U) {
            --s_ui.menu_page;
        }
        return true;
    }

    if (key == '.') {
        return true;
    }

    if (key == ',' || key == '/') {
        return true;
    }

    return false;
}

static ui_input_event_t ui_service_map_normal_char(char ch)
{
    ui_input_event_t event = {
        .type = UI_INPUT_EVENT_NONE,
        .key = ch,
    };

    if (ch == '\n' || ch == '\r') {
        event.type = UI_INPUT_EVENT_SELECT;
    } else if (ch == '\b' || ch == 0x7f) {
        event.type = UI_INPUT_EVENT_BACKSPACE;
    } else if (ch == '`' || ch == '\x1B') {
        event.type = UI_INPUT_EVENT_CANCEL;
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
    } else if ((s_ui.mode == UI_SERVICE_MODE_KEYER || s_ui.mode == UI_SERVICE_MODE_PRACTICE) &&
               ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9'))) {
        event.type = UI_INPUT_EVENT_CHAR_INPUT;
    } else if ((s_ui.mode == UI_SERVICE_MODE_KEYER || s_ui.mode == UI_SERVICE_MODE_PRACTICE) &&
               (ch == '+' || ch == '=')) {
        event.type = UI_INPUT_EVENT_WPM_UP;
    } else if ((s_ui.mode == UI_SERVICE_MODE_KEYER || s_ui.mode == UI_SERVICE_MODE_PRACTICE) &&
               ch == '-') {
        event.type = UI_INPUT_EVENT_WPM_DOWN;
    } else if ((s_ui.mode == UI_SERVICE_MODE_KEYER || s_ui.mode == UI_SERVICE_MODE_PRACTICE) &&
               ch == ']') {
        event.type = UI_INPUT_EVENT_PITCH_UP;
    } else if ((s_ui.mode == UI_SERVICE_MODE_KEYER || s_ui.mode == UI_SERVICE_MODE_PRACTICE) &&
               ch == '[') {
        event.type = UI_INPUT_EVENT_PITCH_DOWN;
    }

    return event;
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
    if (!ui_cardputer_port_poll_input(&port_event)) {
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

    if (port_event.type == UI_CARDPUTER_PORT_EVENT_FN) {
        return UI_EVENT_NONE;
    }

    if (port_event.type != UI_CARDPUTER_PORT_EVENT_CHAR) {
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

        if (ui_service_handle_menu_char(port_event.ch, &menu_event)) {
            if (menu_event.type == UI_INPUT_EVENT_NONE) {
                ui_service_render_current_view();
            }
        }

        if (menu_event.type != UI_INPUT_EVENT_NONE) {
            return menu_event;
        }

        return UI_EVENT_NONE;
    }

    ui_input_event_t event = ui_service_map_normal_char(port_event.ch);

    if (event.type != UI_INPUT_EVENT_NONE) {
        ESP_LOGI(TAG, "input event: type=%d key='%c'", event.type, event.key);
    }

    return event;
}

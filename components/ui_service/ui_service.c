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

#include "audio_service.h"
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
    MINI_CW_MODE_PRACTICE = 0,
    MINI_CW_MODE_KEYER,
    MINI_CW_MODE_LESSONS,
} mini_cw_mode_t;

typedef enum {
    UI_VIEW_NORMAL = 0,
    UI_VIEW_GLOBAL_MENU,
    UI_VIEW_LOCAL_MENU,
} ui_view_t;

typedef enum {
    UI_EDIT_NONE = 0,
    UI_EDIT_TONE,
    UI_EDIT_VOLUME,
    UI_EDIT_KEY_IN_WPM,
    UI_EDIT_KEY_OUT_WPM,
} ui_edit_target_t;

typedef struct {
    mini_cw_mode_t mode;
    ui_view_t view;
    uint8_t global_page;
    uint8_t local_page;
    ui_edit_target_t edit_target;
    uint8_t edit_page;
    uint8_t edit_item;
    char edit_buf[4];
    bool edit_user_digits;
} ui_service_state_t;

static const ui_input_event_t UI_EVENT_NONE = {
    .type = UI_INPUT_EVENT_NONE,
    .key = '\0',
};

static ui_service_state_t s_ui = {
    .mode = MINI_CW_MODE_KEYER,
    .view = UI_VIEW_NORMAL,
    .global_page = 0,
    .local_page = 0,
    .edit_target = UI_EDIT_NONE,
    .edit_page = 0,
    .edit_item = 0,
    .edit_buf = "",
    .edit_user_digits = false,
};

static bool s_cardputer_ready;

#define UI_GLOBAL_PAGE_COUNT 2U
#define UI_LOCAL_PAGE_COUNT 1U

#define UI_TONE_MIN_HZ 300
#define UI_TONE_MAX_HZ 999
#define UI_TONE_STEP_HZ 50
#define UI_VOLUME_MIN 0
#define UI_VOLUME_MAX 99
#define UI_VOLUME_STEP 5
#define UI_WPM_MIN 5
#define UI_WPM_MAX 60
#define UI_WPM_STEP 1

static void ui_service_render_current_view(void);

static const char *ui_service_mode_name(mini_cw_mode_t mode)
{
    switch (mode) {
    case MINI_CW_MODE_PRACTICE:
        return "Practice";
    case MINI_CW_MODE_KEYER:
        return "Keyer";
    case MINI_CW_MODE_LESSONS:
        return "Lessons";
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

static void ui_service_set_uint_text(char *dest,
                                     size_t dest_size,
                                     unsigned value,
                                     unsigned max_value)
{
    char text[11];
    size_t len;

    if (value > max_value) {
        value = max_value;
    }

    snprintf(text, sizeof(text), "%u", value);
    if (dest == NULL || dest_size == 0U) {
        return;
    }

    len = strlen(text);
    if (len >= dest_size) {
        len = dest_size - 1U;
    }

    memcpy(dest, text, len);
    dest[len] = '\0';
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

static int ui_service_edit_min(ui_edit_target_t target)
{
    switch (target) {
    case UI_EDIT_TONE:
        return UI_TONE_MIN_HZ;
    case UI_EDIT_VOLUME:
        return UI_VOLUME_MIN;
    case UI_EDIT_KEY_IN_WPM:
    case UI_EDIT_KEY_OUT_WPM:
        return UI_WPM_MIN;
    case UI_EDIT_NONE:
    default:
        return 0;
    }
}

static int ui_service_edit_max(ui_edit_target_t target)
{
    switch (target) {
    case UI_EDIT_TONE:
        return UI_TONE_MAX_HZ;
    case UI_EDIT_VOLUME:
        return UI_VOLUME_MAX;
    case UI_EDIT_KEY_IN_WPM:
    case UI_EDIT_KEY_OUT_WPM:
        return UI_WPM_MAX;
    case UI_EDIT_NONE:
    default:
        return 0;
    }
}

static int ui_service_edit_step(ui_edit_target_t target)
{
    switch (target) {
    case UI_EDIT_TONE:
        return UI_TONE_STEP_HZ;
    case UI_EDIT_VOLUME:
        return UI_VOLUME_STEP;
    case UI_EDIT_KEY_IN_WPM:
    case UI_EDIT_KEY_OUT_WPM:
        return UI_WPM_STEP;
    case UI_EDIT_NONE:
    default:
        return 1;
    }
}

static size_t ui_service_edit_max_digits(ui_edit_target_t target)
{
    return target == UI_EDIT_TONE ? 3U : 2U;
}

static int ui_service_get_edit_value(ui_edit_target_t target)
{
    switch (target) {
    case UI_EDIT_TONE:
        return audio_service_get_tone_hz();
    case UI_EDIT_VOLUME:
        return audio_service_get_volume();
    case UI_EDIT_KEY_IN_WPM:
        return keyer_service_get_key_in_wpm();
    case UI_EDIT_KEY_OUT_WPM:
        return keyer_service_get_key_out_wpm();
    case UI_EDIT_NONE:
    default:
        return 0;
    }
}

static void ui_service_set_edit_value(ui_edit_target_t target, int value)
{
    value = ui_service_clamp_int(value, ui_service_edit_min(target), ui_service_edit_max(target));

    switch (target) {
    case UI_EDIT_TONE:
        audio_service_set_tone_hz((uint16_t)value);
        break;
    case UI_EDIT_VOLUME:
        audio_service_set_volume((uint8_t)value);
        break;
    case UI_EDIT_KEY_IN_WPM:
        keyer_service_set_key_in_wpm((uint8_t)value);
        break;
    case UI_EDIT_KEY_OUT_WPM:
        keyer_service_set_key_out_wpm((uint8_t)value);
        break;
    case UI_EDIT_NONE:
    default:
        break;
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

static void ui_service_clear_edit(void)
{
    s_ui.edit_target = UI_EDIT_NONE;
    s_ui.edit_page = 0U;
    s_ui.edit_item = 0U;
    s_ui.edit_buf[0] = '\0';
    s_ui.edit_user_digits = false;
}

static bool ui_service_global_item_edit_target(uint8_t page,
                                               uint8_t item,
                                               ui_edit_target_t *out_target)
{
    ui_edit_target_t target = UI_EDIT_NONE;

    if (page == 0U) {
        if (item == 2U) {
            target = UI_EDIT_TONE;
        } else if (item == 3U) {
            target = UI_EDIT_VOLUME;
        } else if (item == 5U) {
            target = UI_EDIT_KEY_IN_WPM;
        }
    } else if (page == 1U && item == 2U) {
        target = UI_EDIT_KEY_OUT_WPM;
    }

    if (out_target != NULL) {
        *out_target = target;
    }

    return target != UI_EDIT_NONE;
}

static bool ui_service_is_editing_item(uint8_t page, uint8_t item)
{
    return s_ui.edit_target != UI_EDIT_NONE && s_ui.edit_page == page && s_ui.edit_item == item;
}

static void ui_service_begin_numeric_edit(uint8_t item)
{
    ui_edit_target_t target;

    if (!ui_service_global_item_edit_target(s_ui.global_page, item, &target)) {
        return;
    }

    s_ui.edit_target = target;
    s_ui.edit_page = s_ui.global_page;
    s_ui.edit_item = item;
    s_ui.edit_user_digits = false;
    ui_service_set_edit_buf_value(ui_service_get_edit_value(target));
    ESP_LOGI(TAG, "global menu edit started: page=%u item=%u", s_ui.edit_page, s_ui.edit_item);
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

static void ui_service_step_numeric_edit(int delta)
{
    int step;

    if (s_ui.edit_target == UI_EDIT_NONE) {
        return;
    }

    step = ui_service_edit_step(s_ui.edit_target);

    switch (s_ui.edit_target) {
    case UI_EDIT_TONE:
        audio_service_adjust_tone_hz(delta * step);
        break;
    case UI_EDIT_VOLUME:
        audio_service_adjust_volume(delta * step);
        audio_service_play_feedback_beep();
        break;
    case UI_EDIT_KEY_IN_WPM:
        keyer_service_adjust_key_in_wpm(delta * step);
        break;
    case UI_EDIT_KEY_OUT_WPM:
        keyer_service_adjust_key_out_wpm(delta * step);
        break;
    case UI_EDIT_NONE:
    default:
        break;
    }

    ui_service_set_edit_buf_value(ui_service_get_edit_value(s_ui.edit_target));
    s_ui.edit_user_digits = false;
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

static ui_edit_target_t ui_service_commit_numeric_edit(void)
{
    ui_edit_target_t target = s_ui.edit_target;
    int value;

    if (target == UI_EDIT_NONE) {
        return UI_EDIT_NONE;
    }

    value = ui_service_edit_buffer_value();
    ui_service_set_edit_value(target, value);
    ESP_LOGI(TAG, "global menu edit committed: value=%d", ui_service_get_edit_value(target));
    ui_service_clear_edit();
    return target;
}

static bool ui_service_handle_edit_char(char key)
{
    if (s_ui.edit_target == UI_EDIT_NONE) {
        return false;
    }

    if (key == '\n' || key == '\r') {
        if (ui_service_commit_numeric_edit() == UI_EDIT_VOLUME) {
            audio_service_play_feedback_beep();
        }

        return true;
    }

    if (key == '`' || key == '\x1B') {
        ESP_LOGI(TAG, "global menu edit canceled");
        ui_service_clear_edit();
        return true;
    }

    if (key == '\b' || key == 0x7f) {
        ui_service_backspace_edit_digit();
        return true;
    }

    if (key == ',') {
        ui_service_step_numeric_edit(-1);
        return true;
    }

    if (key == '/') {
        ui_service_step_numeric_edit(1);
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

static void ui_service_prepare_screen(mini_cw_screen_t *screen)
{
    if (screen == NULL) {
        return;
    }

    memset(screen, 0, sizeof(*screen));
    ui_service_set_text(screen->mode, sizeof(screen->mode), ui_service_mode_name(s_ui.mode));
    ui_service_set_uint_text(screen->tone,
                             sizeof(screen->tone),
                             audio_service_get_tone_hz(),
                             UI_TONE_MAX_HZ);
    ui_service_set_uint_text(screen->vol,
                             sizeof(screen->vol),
                             audio_service_get_volume(),
                             UI_VOLUME_MAX);
    ui_service_set_text(screen->key_in,
                        sizeof(screen->key_in),
                        keyer_service_io_mode_label(keyer_service_get_key_in_mode()));
    ui_service_set_text(screen->key_out,
                        sizeof(screen->key_out),
                        keyer_service_io_mode_label(keyer_service_get_key_out_mode()));
    ui_service_set_uint_text(screen->key_wpm,
                             sizeof(screen->key_wpm),
                             keyer_service_get_key_in_wpm(),
                             UI_WPM_MAX);
}

static void ui_service_format_global_value_line(char *dest,
                                                size_t dest_size,
                                                uint8_t page,
                                                uint8_t item,
                                                const char *prefix,
                                                int value,
                                                const char *suffix)
{
    if (ui_service_is_editing_item(page, item)) {
        if (s_ui.edit_buf[0] == '\0') {
            snprintf(dest, dest_size, "%s_%s", prefix, suffix ? suffix : "");
        } else {
            snprintf(dest, dest_size, "%s%s%s_", prefix, s_ui.edit_buf, suffix ? suffix : "");
        }
    } else {
        snprintf(dest, dest_size, "%s%d%s", prefix, value, suffix ? suffix : "");
    }
}

static int ui_service_read_battery_percent(void)
{
    int percent = 0;

    if (platform_hal_get_battery_percent(&percent) != ESP_OK) {
        return 0;
    }

    return ui_service_clamp_int(percent, 0, 100);
}

static void ui_service_render_normal(void)
{
    mini_cw_screen_t screen;

    ui_service_prepare_screen(&screen);
    ui_screen_render(&screen);
}

static void ui_service_render_global_menu(void)
{
    mini_cw_screen_t screen;

    ui_service_prepare_screen(&screen);

    if (s_ui.global_page == 0U) {
        snprintf(screen.line[0],
                 sizeof(screen.line[0]),
                 "1 Mode:%s",
                 ui_service_mode_name(s_ui.mode));
        ui_service_format_global_value_line(screen.line[1],
                                            sizeof(screen.line[1]),
                                            0U,
                                            2U,
                                            "2 Tone:",
                                            audio_service_get_tone_hz(),
                                            "");
        ui_service_format_global_value_line(screen.line[2],
                                            sizeof(screen.line[2]),
                                            0U,
                                            3U,
                                            "3 Volume:",
                                            audio_service_get_volume(),
                                            "");
        snprintf(screen.line[3],
                 sizeof(screen.line[3]),
                 "4 KeyIn:%s",
                 keyer_service_io_mode_label(keyer_service_get_key_in_mode()));
        ui_service_format_global_value_line(screen.line[4],
                                            sizeof(screen.line[4]),
                                            0U,
                                            5U,
                                            "5 KeyIn WPM:",
                                            keyer_service_get_key_in_wpm(),
                                            "");
    } else {
        snprintf(screen.line[0],
                 sizeof(screen.line[0]),
                 "1 KeyOut:%s",
                 keyer_service_io_mode_label(keyer_service_get_key_out_mode()));
        ui_service_format_global_value_line(screen.line[1],
                                            sizeof(screen.line[1]),
                                            1U,
                                            2U,
                                            "2 KeyOut WPM:",
                                            keyer_service_get_key_out_wpm(),
                                            "");
        snprintf(screen.line[2],
                 sizeof(screen.line[2]),
                 "3 Sleep/Batt %d%%",
                 ui_service_read_battery_percent());
        ui_service_set_text(screen.line[3], sizeof(screen.line[3]), "4 Date");
        ui_service_set_text(screen.line[4], sizeof(screen.line[4]), "5 Time");
    }

    ui_screen_render(&screen);
}

static void ui_service_render_local_no_settings(void)
{
    mini_cw_screen_t screen;

    ui_service_prepare_screen(&screen);
    ui_service_set_text(screen.line[0], sizeof(screen.line[0]), "No local settings");

    ui_screen_render(&screen);
}

static void ui_service_render_local_stub(void)
{
    mini_cw_screen_t screen;

    ui_service_prepare_screen(&screen);
    ui_service_set_text(screen.line[0], sizeof(screen.line[0]), "1 Local:stub");
    ui_service_set_text(screen.line[1], sizeof(screen.line[1]), "2 Mode only");
    ui_service_set_text(screen.line[2], sizeof(screen.line[2]), "3");
    ui_service_set_text(screen.line[3], sizeof(screen.line[3]), "4");
    ui_service_set_text(screen.line[4], sizeof(screen.line[4]), "5");

    ui_screen_render(&screen);
}

static void ui_service_render_local_menu(void)
{
    switch (s_ui.mode) {
    case MINI_CW_MODE_PRACTICE:
        ui_service_render_local_no_settings();
        break;
    case MINI_CW_MODE_KEYER:
    case MINI_CW_MODE_LESSONS:
    default:
        ui_service_render_local_stub();
        break;
    }
}

static void ui_service_render_current_view(void)
{
    switch (s_ui.view) {
    case UI_VIEW_GLOBAL_MENU:
        ui_service_render_global_menu();
        break;
    case UI_VIEW_LOCAL_MENU:
        ui_service_render_local_menu();
        break;
    case UI_VIEW_NORMAL:
    default:
        ui_service_render_normal();
        break;
    }
}

static void ui_service_change_menu_page(int delta)
{
    uint8_t *page = NULL;
    uint8_t page_count = 1U;

    if (s_ui.view == UI_VIEW_GLOBAL_MENU) {
        page = &s_ui.global_page;
        page_count = UI_GLOBAL_PAGE_COUNT;
    } else if (s_ui.view == UI_VIEW_LOCAL_MENU) {
        page = &s_ui.local_page;
        page_count = UI_LOCAL_PAGE_COUNT;
    }

    if (page == NULL) {
        return;
    }

    if (delta < 0 && *page > 0U) {
        --(*page);
    } else if (delta > 0 && (uint8_t)(*page + 1U) < page_count) {
        ++(*page);
    }
}

static bool ui_service_handle_menu_char(char key, ui_input_event_t *out_event)
{
    if (out_event != NULL) {
        *out_event = UI_EVENT_NONE;
    }

    if (ui_service_handle_edit_char(key)) {
        return true;
    }

    if (key >= '1' && key <= '5') {
        uint8_t item = (uint8_t)(key - '0');

        if (s_ui.view == UI_VIEW_GLOBAL_MENU &&
            ui_service_global_item_edit_target(s_ui.global_page, item, NULL)) {
            ui_service_begin_numeric_edit(item);
            return true;
        }

        if (s_ui.view == UI_VIEW_GLOBAL_MENU && s_ui.global_page == 0U && item == 4U) {
            keyer_service_cycle_key_in_mode(1);
            return true;
        }

        if (s_ui.view == UI_VIEW_GLOBAL_MENU && s_ui.global_page == 1U && item == 1U) {
            keyer_service_cycle_key_out_mode(1);
            return true;
        }

        if (s_ui.view == UI_VIEW_GLOBAL_MENU && s_ui.global_page == 1U && item == 3U) {
            if (out_event != NULL) {
                out_event->type = UI_INPUT_EVENT_SLEEP_REQUEST;
                out_event->key = key;
            }

            ESP_LOGI(TAG, "sleep requested from global menu");
            return true;
        }

        ESP_LOGI(TAG,
                 "menu item %c selected on view=%u page=%u",
                 key,
                 (unsigned)s_ui.view,
                 (unsigned)(s_ui.view == UI_VIEW_GLOBAL_MENU ? s_ui.global_page
                                                              : s_ui.local_page));
        return true;
    }

    // Menu navigation uses ; . , /. U/D/L/R are reserved for future shortcuts.
    if (key == ';') {
        ui_service_change_menu_page(-1);
        return true;
    }

    if (key == '.') {
        ui_service_change_menu_page(1);
        return true;
    }

    if (key == ',') {
        ESP_LOGI(TAG, "menu left/change-value stub");
        return true;
    }

    if (key == '/') {
        ESP_LOGI(TAG, "menu right/change-value stub");
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

    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9')) {
        event.type = UI_INPUT_EVENT_CHAR_INPUT;
    } else if (ch == '+' || ch == '=') {
        event.type = UI_INPUT_EVENT_WPM_UP;
    } else if (ch == '-') {
        event.type = UI_INPUT_EVENT_WPM_DOWN;
    } else if (ch == ']') {
        event.type = UI_INPUT_EVENT_PITCH_UP;
    } else if (ch == '[') {
        event.type = UI_INPUT_EVENT_PITCH_DOWN;
    } else if (ch == '`' || ch == '\x1B') {
        event.type = UI_INPUT_EVENT_CANCEL;
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

void ui_service_enter_global_menu(void)
{
    s_ui.view = UI_VIEW_GLOBAL_MENU;
    s_ui.global_page = 0U;
    ui_service_clear_edit();
    ESP_LOGI(TAG, "global menu entered");
    ui_service_render_current_view();
}

void ui_service_exit_global_menu(void)
{
    if (s_ui.view == UI_VIEW_GLOBAL_MENU) {
        s_ui.view = UI_VIEW_NORMAL;
    }

    ui_service_clear_edit();
    ESP_LOGI(TAG, "global menu exited");
    ui_service_render_current_view();
}

void ui_service_enter_local_menu(void)
{
    s_ui.view = UI_VIEW_LOCAL_MENU;
    s_ui.local_page = 0U;
    ui_service_clear_edit();
    ESP_LOGI(TAG, "local menu entered");
    ui_service_render_current_view();
}

void ui_service_exit_local_menu(void)
{
    if (s_ui.view == UI_VIEW_LOCAL_MENU) {
        s_ui.view = UI_VIEW_NORMAL;
    }

    ui_service_clear_edit();
    ESP_LOGI(TAG, "local menu exited");
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

    if (port_event.type == UI_CARDPUTER_PORT_EVENT_CTRL) {
        if (s_ui.view == UI_VIEW_GLOBAL_MENU) {
            ui_service_exit_global_menu();
        } else if (s_ui.view == UI_VIEW_NORMAL) {
            ui_service_enter_global_menu();
        } else {
            ESP_LOGI(TAG, "ctrl ignored in local menu");
        }

        return UI_EVENT_NONE;
    }

    if (port_event.type == UI_CARDPUTER_PORT_EVENT_FN) {
        if (s_ui.view == UI_VIEW_LOCAL_MENU) {
            ui_service_exit_local_menu();
        } else if (s_ui.view == UI_VIEW_NORMAL) {
            ui_service_enter_local_menu();
        } else {
            ESP_LOGI(TAG, "fn ignored in global menu");
        }

        return UI_EVENT_NONE;
    }

    if (port_event.type != UI_CARDPUTER_PORT_EVENT_CHAR) {
        return UI_EVENT_NONE;
    }

    if (s_ui.view == UI_VIEW_GLOBAL_MENU || s_ui.view == UI_VIEW_LOCAL_MENU) {
        ui_input_event_t menu_event = UI_EVENT_NONE;

        if (ui_service_handle_menu_char(port_event.ch, &menu_event)) {
            ui_service_render_current_view();
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

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

#include "esp_log.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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

typedef struct {
    mini_cw_mode_t mode;
    ui_view_t view;
    uint8_t global_page;
    uint8_t local_page;
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
};

static bool s_cardputer_ready;

#define UI_GLOBAL_PAGE_COUNT 2U
#define UI_LOCAL_PAGE_COUNT 1U

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

static void ui_service_prepare_screen(mini_cw_screen_t *screen)
{
    if (screen == NULL) {
        return;
    }

    memset(screen, 0, sizeof(*screen));
    ui_service_set_text(screen->mode, sizeof(screen->mode), ui_service_mode_name(s_ui.mode));
    ui_service_set_text(screen->tone, sizeof(screen->tone), "700");
    ui_service_set_text(screen->vol, sizeof(screen->vol), "40");
    ui_service_set_text(screen->key_in, sizeof(screen->key_in), "Paddle");
    ui_service_set_text(screen->key_out, sizeof(screen->key_out), "Paddle");
    ui_service_set_text(screen->key_wpm, sizeof(screen->key_wpm), "20");
}

static void ui_service_render_normal(void)
{
    mini_cw_screen_t screen;

    ui_service_prepare_screen(&screen);
    ui_service_set_text(screen.line[0], sizeof(screen.line[0]), "CQ CQ DE AG6AQ");
    ui_service_set_text(screen.line[1], sizeof(screen.line[1]), "BUF:");
    ui_service_set_text(screen.line[2], sizeof(screen.line[2]), "KEYIN:Paddle");
    ui_service_set_text(screen.line[3], sizeof(screen.line[3]), "KEYOUT:Paddle");
    ui_service_set_text(screen.line[4], sizeof(screen.line[4]), "READY");

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
        ui_service_set_text(screen.line[1], sizeof(screen.line[1]), "2 Tone:700Hz");
        ui_service_set_text(screen.line[2], sizeof(screen.line[2]), "3 Volume:40");
        ui_service_set_text(screen.line[3], sizeof(screen.line[3]), "4 KeyIn:Paddle");
        ui_service_set_text(screen.line[4], sizeof(screen.line[4]), "5 KeyIn WPM:20");
    } else {
        ui_service_set_text(screen.line[0], sizeof(screen.line[0]), "1 KeyOut:Paddle");
        ui_service_set_text(screen.line[1], sizeof(screen.line[1]), "2 KeyOut WPM:20");
        ui_service_set_text(screen.line[2], sizeof(screen.line[2]), "3 Sleep/Batt 90%");
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

static bool ui_service_handle_menu_char(char key)
{
    if (key >= '1' && key <= '5') {
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
    ui_service_render_current_view();
}

void ui_service_enter_global_menu(void)
{
    s_ui.view = UI_VIEW_GLOBAL_MENU;
    s_ui.global_page = 0U;
    ESP_LOGI(TAG, "global menu entered");
    ui_service_render_current_view();
}

void ui_service_exit_global_menu(void)
{
    if (s_ui.view == UI_VIEW_GLOBAL_MENU) {
        s_ui.view = UI_VIEW_NORMAL;
    }

    ESP_LOGI(TAG, "global menu exited");
    ui_service_render_current_view();
}

void ui_service_enter_local_menu(void)
{
    s_ui.view = UI_VIEW_LOCAL_MENU;
    s_ui.local_page = 0U;
    ESP_LOGI(TAG, "local menu entered");
    ui_service_render_current_view();
}

void ui_service_exit_local_menu(void)
{
    if (s_ui.view == UI_VIEW_LOCAL_MENU) {
        s_ui.view = UI_VIEW_NORMAL;
    }

    ESP_LOGI(TAG, "local menu exited");
    ui_service_render_current_view();
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
        if (ui_service_handle_menu_char(port_event.ch)) {
            ui_service_render_current_view();
        }

        return UI_EVENT_NONE;
    }

    ui_input_event_t event = ui_service_map_normal_char(port_event.ch);

    if (event.type != UI_INPUT_EVENT_NONE) {
        ESP_LOGI(TAG, "input event: type=%d key='%c'", event.type, event.key);
    }

    return event;
}

/*
 * ui_cardputer_port
 *
 * Responsibility: Initializes the Cardputer UI port, provides low-level
 * display primitives, and reads keyboard input via the known-good mic_test
 * M5Cardputer component path.
 * Hardware ownership: display and keyboard. ui_service is the public owner;
 * this private port is the only place that initializes Cardputer UI hardware
 * or touches keyboard/display APIs.
 */

#include "ui_cardputer_port.h"

#include "M5Cardputer.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "ui_cardputer_port";

static bool s_initialized = false;
static char s_last_reported_key = '\0';
static bool s_last_reported_key_fn = false;
static bool s_last_reported_key_shift = false;
static bool s_last_reported_key_ctrl = false;
static bool s_last_reported_key_opt = false;
static bool s_last_reported_key_alt = false;
static bool s_last_reported_key_caps_lock = false;
static bool s_last_reported_fn = false;
static bool s_last_reported_ctrl = false;
static bool s_last_reported_opt = false;
static bool s_last_reported_alt = false;
static int64_t s_backspace_press_us = 0;
static bool s_backspace_hold_reported = false;

#define UI_CARDPUTER_BACKSPACE_HOLD_US 1000000LL

static uint16_t ui_cardputer_port_map_color(ui_cardputer_port_color_t color)
{
    switch (color) {
    case UI_CARDPUTER_PORT_COLOR_WHITE:
        return TFT_WHITE;
    case UI_CARDPUTER_PORT_COLOR_GREEN:
        return TFT_GREEN;
    case UI_CARDPUTER_PORT_COLOR_CYAN:
        return TFT_CYAN;
    case UI_CARDPUTER_PORT_COLOR_BLACK:
    default:
        return TFT_BLACK;
    }
}

bool ui_cardputer_port_init(void)
{
    if (s_initialized) {
        return true;
    }

    M5Cardputer.beginDisplayOnly(true);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.fillScreen(TFT_BLACK);
    M5Cardputer.Display.setTextSize(2);
    M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5Cardputer.Display.setTextScroll(false);

    s_initialized = true;
    ESP_LOGI(TAG, "M5Cardputer display/keyboard initialized via mic_test component path");
    return true;
}

bool ui_cardputer_port_poll_input(ui_cardputer_port_event_t *out_event)
{
    if (out_event == nullptr) {
        return false;
    }

    out_event->type = UI_CARDPUTER_PORT_EVENT_NONE;
    out_event->ch = '\0';
    out_event->fn = false;
    out_event->shift = false;
    out_event->ctrl = false;
    out_event->opt = false;
    out_event->alt = false;
    out_event->caps_lock = false;

    if (!s_initialized) {
        return false;
    }

    M5Cardputer.update();

    auto &keys = M5Cardputer.Keyboard.keysState();
    char ch = '\0';
    int64_t now_us = esp_timer_get_time();

    out_event->fn = keys.fn;
    out_event->shift = keys.shift;
    out_event->ctrl = keys.ctrl;
    out_event->opt = keys.opt;
    out_event->alt = keys.alt;
    out_event->caps_lock = M5Cardputer.Keyboard.capslocked();

    if (keys.del) {
        if (s_backspace_press_us == 0) {
            s_backspace_press_us = now_us;
            s_backspace_hold_reported = false;
        } else if (!s_backspace_hold_reported &&
                   now_us - s_backspace_press_us >= UI_CARDPUTER_BACKSPACE_HOLD_US) {
            s_backspace_hold_reported = true;
            out_event->type = UI_CARDPUTER_PORT_EVENT_BACKSPACE_HOLD;
            ESP_LOGI(TAG, "keyboard backspace hold");
            return true;
        }
    } else {
        s_backspace_press_us = 0;
        s_backspace_hold_reported = false;
    }

    if (!keys.word.empty()) {
        ch = keys.word.front();
    } else if (keys.del) {
        ch = '\b';
    } else if (keys.enter) {
        ch = '\n';
    } else if (keys.tab) {
        ch = '\t';
    }

    if (ch != '\0') {
        out_event->ch = ch;
        s_last_reported_fn = false;
        s_last_reported_ctrl = false;
        s_last_reported_opt = false;
        s_last_reported_alt = false;

        if (ch == s_last_reported_key && keys.fn == s_last_reported_key_fn &&
            keys.shift == s_last_reported_key_shift && keys.ctrl == s_last_reported_key_ctrl &&
            keys.opt == s_last_reported_key_opt && keys.alt == s_last_reported_key_alt &&
            out_event->caps_lock == s_last_reported_key_caps_lock) {
            return false;
        }

        s_last_reported_key = ch;
        s_last_reported_key_fn = keys.fn;
        s_last_reported_key_shift = keys.shift;
        s_last_reported_key_ctrl = keys.ctrl;
        s_last_reported_key_opt = keys.opt;
        s_last_reported_key_alt = keys.alt;
        s_last_reported_key_caps_lock = out_event->caps_lock;
        out_event->type = UI_CARDPUTER_PORT_EVENT_CHAR;
        ESP_LOGI(TAG, "keyboard char: '%c'", ch);
        return true;
    }

    s_last_reported_key = '\0';

    if (keys.opt) {
        s_last_reported_fn = false;
        s_last_reported_ctrl = false;
        s_last_reported_alt = false;

        if (s_last_reported_opt) {
            return false;
        }

        s_last_reported_opt = true;
        out_event->type = UI_CARDPUTER_PORT_EVENT_OPT;
        ESP_LOGI(TAG, "keyboard opt");
        return true;
    }

    s_last_reported_opt = false;

    if (keys.alt) {
        s_last_reported_fn = false;
        s_last_reported_ctrl = false;
        s_last_reported_opt = false;

        if (s_last_reported_alt) {
            return false;
        }

        s_last_reported_alt = true;
        out_event->type = UI_CARDPUTER_PORT_EVENT_ALT;
        ESP_LOGI(TAG, "keyboard alt");
        return true;
    }

    s_last_reported_alt = false;

    if (keys.fn) {
        s_last_reported_ctrl = false;
        s_last_reported_opt = false;
        s_last_reported_alt = false;

        if (s_last_reported_fn) {
            return false;
        }

        s_last_reported_fn = true;
        out_event->type = UI_CARDPUTER_PORT_EVENT_FN;
        ESP_LOGI(TAG, "keyboard fn");
        return true;
    }

    s_last_reported_fn = false;

    if (keys.ctrl) {
        s_last_reported_opt = false;
        s_last_reported_alt = false;

        if (s_last_reported_ctrl) {
            return false;
        }

        s_last_reported_ctrl = true;
        out_event->type = UI_CARDPUTER_PORT_EVENT_CTRL;
        ESP_LOGI(TAG, "keyboard ctrl");
        return true;
    }

    s_last_reported_ctrl = false;
    return false;
}

void ui_cardputer_port_display_begin_frame(void)
{
    if (!s_initialized) {
        return;
    }

    auto &display = M5Cardputer.Display;

    display.startWrite();
    display.setRotation(1);
    display.setTextScroll(false);
    display.setTextSize(2);
}

void ui_cardputer_port_display_end_frame(void)
{
    if (!s_initialized) {
        return;
    }

    M5Cardputer.Display.endWrite();
}

void ui_cardputer_port_display_fill_screen(ui_cardputer_port_color_t color)
{
    if (!s_initialized) {
        return;
    }

    M5Cardputer.Display.fillScreen(ui_cardputer_port_map_color(color));
}

void ui_cardputer_port_display_fill_rect(int x,
                                         int y,
                                         int width,
                                         int height,
                                         ui_cardputer_port_color_t color)
{
    if (!s_initialized) {
        return;
    }

    M5Cardputer.Display.fillRect(x, y, width, height, ui_cardputer_port_map_color(color));
}

void ui_cardputer_port_display_print_text(int x,
                                          int y,
                                          const char *text,
                                          ui_cardputer_port_color_t fg,
                                          ui_cardputer_port_color_t bg)
{
    if (!s_initialized) {
        return;
    }

    auto &display = M5Cardputer.Display;

    display.setTextColor(ui_cardputer_port_map_color(fg), ui_cardputer_port_map_color(bg));
    display.setCursor(x, y);
    display.print(text ? text : "");
}

void ui_cardputer_port_display_sleep(void)
{
    if (!s_initialized) {
        return;
    }

    M5Cardputer.Display.sleep();
}

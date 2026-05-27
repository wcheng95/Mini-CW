/*
 * ui_cardputer_port
 *
 * Responsibility: Draws Mini-CW screens and reads Cardputer keyboard input via
 * the known-good mic_test M5Cardputer component.
 * Hardware ownership: display and keyboard. ui_service is the public owner;
 * this private port is the only place that touches M5Cardputer APIs.
 */

#include "ui_cardputer_port.h"

#include "M5Cardputer.h"
#include "esp_log.h"

#include <cctype>
#include <cstdio>

static const char *TAG = "ui_cardputer_port";

static bool s_initialized = false;
static char s_last_reported_key = '\0';

static constexpr int SCREEN_W = 240;
static constexpr int LINE_H = 19;
static constexpr int TEXT_X = 4;

static void draw_line(int y, const char *text, uint16_t color)
{
    auto &display = M5Cardputer.Display;
    display.setTextColor(color, TFT_BLACK);
    display.fillRect(0, y, SCREEN_W, LINE_H, TFT_BLACK);
    display.setCursor(TEXT_X, y);
    display.print(text ? text : "");
}

static void draw_status_screen(const char *title,
                               const char *mode,
                               const char *line1,
                               const char *line2,
                               const char *line3,
                               const char *line4)
{
    auto &display = M5Cardputer.Display;

    display.startWrite();
    display.fillScreen(TFT_BLACK);
    display.setRotation(1);
    display.setTextSize(2);
    display.setTextScroll(false);

    draw_line(0, title, TFT_YELLOW);
    draw_line(LINE_H, mode, TFT_WHITE);
    draw_line(LINE_H * 2, line1, TFT_CYAN);
    draw_line(LINE_H * 3, line2, TFT_GREEN);
    draw_line(LINE_H * 4, line3, TFT_WHITE);
    draw_line(LINE_H * 5, line4, TFT_RED);

    display.endWrite();
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

void ui_cardputer_port_show_home(const char *mode_name, const char *status)
{
    if (!s_initialized) {
        return;
    }

    char mode_line[32];
    char status_line[32];
    std::snprintf(mode_line, sizeof(mode_line), "Mode: %s", mode_name ? mode_name : "Unknown");
    std::snprintf(status_line, sizeof(status_line), "%s", status ? status : "Ready");

    draw_status_screen("Mini-CW",
                       mode_line,
                       status_line,
                       "A-Z 0-9",
                       "+-WPM []Hz",
                       "` Stop");
}

void ui_cardputer_port_show_tone_test(const ui_tone_test_view_t *view)
{
    if (!s_initialized) {
        return;
    }

    const char *mode = (view && view->mode_name) ? view->mode_name : "Tone Test";
    const char *pattern = (view && view->last_pattern) ? view->last_pattern : "";
    const char *status = (view && view->status) ? view->status : "Ready";
    char last = (view && view->last_char) ? view->last_char : '-';
    uint8_t wpm = view ? view->wpm : 20;
    uint16_t pitch_hz = view ? view->pitch_hz : 700;

    if (last >= 'a' && last <= 'z') {
        last = (char)std::toupper((unsigned char)last);
    }

    char mode_line[32];
    char last_line[32];
    char settings_line[32];
    char status_line[32];

    std::snprintf(mode_line, sizeof(mode_line), "%s", mode);
    std::snprintf(last_line, sizeof(last_line), "Last: %c  %s", last, pattern);
    std::snprintf(settings_line,
                  sizeof(settings_line),
                  "%uWPM %uHz",
                  (unsigned)wpm,
                  (unsigned)pitch_hz);
    std::snprintf(status_line, sizeof(status_line), "`Stop %s", status);

    draw_status_screen("Mini-CW Tone",
                       mode_line,
                       "Press A-Z 0-9",
                       last_line,
                       settings_line,
                       status_line);
}

bool ui_cardputer_port_poll_char(char *out_char)
{
    if (out_char == nullptr) {
        return false;
    }

    *out_char = '\0';

    if (!s_initialized) {
        return false;
    }

    M5Cardputer.update();

    auto &keys = M5Cardputer.Keyboard.keysState();
    char ch = '\0';

    if (!keys.word.empty()) {
        ch = keys.word.front();
    } else if (keys.del) {
        ch = '\b';
    } else if (keys.enter) {
        ch = '\n';
    } else if (keys.tab) {
        ch = '\t';
    }

    if (ch == '\0') {
        s_last_reported_key = '\0';
        return false;
    }

    if (ch == s_last_reported_key) {
        return false;
    }

    s_last_reported_key = ch;
    *out_char = ch;
    ESP_LOGI(TAG, "keyboard char: '%c'", ch);
    return true;
}

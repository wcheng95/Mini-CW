/*
 * ui_service
 *
 * Responsibility: Owns screen drawing and keyboard/input event abstraction.
 * Hardware ownership: display and Cardputer keyboard/input. Milestone 2 uses
 * private display and keyboard ports behind this service.
 */

#include "ui_service.h"

#include "audio_service.h"
#include "keyer_service.h"
#include "ui_cardputer_port.h"
#include "ui_screen.h"

#include "esp_log.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "ui_service";

static const char *UI_TOP_BAR = "M:Keyer     Setting";

static const ui_input_event_t UI_EVENT_NONE = {
    .type = UI_INPUT_EVENT_NONE,
    .key = '\0',
};

static const char *s_status = "Ready";
static bool s_cardputer_ready;

static void ui_service_set_line(char dest[UI_COLS + 1], const char *text)
{
    snprintf(dest, UI_COLS + 1, "%s", text ? text : "");
}

static void ui_service_prepare_chrome(mini_cw_screen_t *screen)
{
    if (screen == NULL) {
        return;
    }

    unsigned tx_wpm = keyer_service_get_tx_wpm();
    unsigned tone_hz = audio_service_get_tone_hz();
    unsigned volume = audio_service_get_volume();

    /*
     * The bottom status row is fixed at 20 visible characters. These display
     * bounds keep the formatted service values inside that row.
     */
    if (tx_wpm > 99U) {
        tx_wpm = 99U;
    }
    if (tone_hz > 1000U) {
        tone_hz = 1000U;
    }
    if (volume > 100U) {
        volume = 100U;
    }

    memset(screen, 0, sizeof(*screen));
    ui_service_set_line(screen->top, UI_TOP_BAR);

    /*
     * ui_service composes display text from service-owned values. TX WPM
     * belongs to keyer_service; tone and volume belong to audio_service.
     */
    snprintf(screen->bottom,
             UI_COLS + 1,
             "TX:%u T:%uHz V:%u",
             tx_wpm,
             tone_hz,
             volume);
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
    mini_cw_screen_t screen;

    ui_service_prepare_chrome(&screen);
    ui_service_set_line(screen.line[0], "CQ CQ DE AG6AQ");
    ui_service_set_line(screen.line[1], "BUF:");
    ui_service_set_line(screen.line[2], "KEY:IAMBIC B");
    ui_service_set_line(screen.line[3], "OUT:GPIO??");
    ui_service_set_line(screen.line[4], "READY");

    ui_screen_render(&screen);
}

void ui_service_show_home(const char *mode_name)
{
    const char *display_mode = mode_name ? mode_name : "Unknown";
    mini_cw_screen_t screen;

    ESP_LOGI(TAG, "screen: Mini-CW");
    ESP_LOGI(TAG, "screen: Mode: %s", display_mode);
    ESP_LOGI(TAG, "screen: Status: %s", s_status);
    ESP_LOGI(TAG, "screen: R RX  T TX  C Call  Q QSO");
    ESP_LOGI(TAG, "screen: S Stats  M Menu  Esc Stop");

    ui_service_prepare_chrome(&screen);
    snprintf(screen.line[0], UI_COLS + 1, "Mode:%s", display_mode);
    snprintf(screen.line[1], UI_COLS + 1, "Status:%s", s_status ? s_status : "");
    ui_service_set_line(screen.line[2], "A-Z 0-9");
    ui_service_set_line(screen.line[3], "+-WPM []Hz");
    ui_service_set_line(screen.line[4], "` Stop");

    ui_screen_render(&screen);
}

void ui_service_show_tone_test(const ui_tone_test_view_t *view)
{
    const char *mode = (view && view->mode_name) ? view->mode_name : "Tone Test";
    const char *pattern = (view && view->last_pattern) ? view->last_pattern : "";
    const char *status = (view && view->status) ? view->status : "Ready";
    char last = (view && view->last_char) ? view->last_char : '-';
    uint8_t wpm = view ? view->wpm : 20;
    uint16_t pitch_hz = view ? view->pitch_hz : 700;
    mini_cw_screen_t screen;

    if (last >= 'a' && last <= 'z') {
        last = (char)toupper((unsigned char)last);
    }

    ESP_LOGI(TAG, "screen: Mini-CW Tone Test");
    ESP_LOGI(TAG, "screen: Mode: %s", mode);
    ESP_LOGI(TAG, "screen: Press A-Z / 0-9");
    ESP_LOGI(TAG, "screen: Last: %c  %s", last, pattern);
    ESP_LOGI(TAG, "screen: WPM: %u  Pitch: %u", (unsigned)wpm, (unsigned)pitch_hz);
    ESP_LOGI(TAG, "screen: +/- WPM  [] pitch  ` stop");
    ESP_LOGI(TAG, "screen: Status: %s", status);

    ui_service_prepare_chrome(&screen);
    snprintf(screen.line[0], UI_COLS + 1, "Mode:%s", mode);
    ui_service_set_line(screen.line[1], "Press A-Z 0-9");
    snprintf(screen.line[2], UI_COLS + 1, "Last:%c %s", last, pattern);
    snprintf(screen.line[3], UI_COLS + 1, "%uWPM %uHz", (unsigned)wpm, (unsigned)pitch_hz);
    snprintf(screen.line[4], UI_COLS + 1, "`Stop %s", status);

    ui_screen_render(&screen);
}

void ui_service_set_status(const char *status)
{
    s_status = status ? status : "";
    ESP_LOGI(TAG, "status: %s", s_status);
}

ui_input_event_t ui_service_poll_input(void)
{
    char ch = '\0';
    if (!ui_cardputer_port_poll_char(&ch)) {
        return UI_EVENT_NONE;
    }

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

    if (event.type != UI_INPUT_EVENT_NONE) {
        ESP_LOGI(TAG, "input event: type=%d key='%c'", event.type, ch);
    }

    return event;
}

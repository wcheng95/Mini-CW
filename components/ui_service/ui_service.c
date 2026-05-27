/*
 * ui_service
 *
 * Responsibility: Owns screen drawing and keyboard/input event abstraction.
 * Hardware ownership: display and Cardputer keyboard/input. Milestone 2 uses
 * private display and keyboard ports behind this service.
 */

#include "ui_service.h"

#include "ui_cardputer_port.h"

#include "esp_log.h"

static const char *TAG = "ui_service";

static const ui_input_event_t UI_EVENT_NONE = {
    .type = UI_INPUT_EVENT_NONE,
    .key = '\0',
};

static const char *s_status = "Ready";
static bool s_cardputer_ready;

void ui_service_init(void)
{
    s_cardputer_ready = ui_cardputer_port_init();
    ESP_LOGI(TAG,
             "display/keyboard owner: %s",
             s_cardputer_ready ? "M5Cardputer mic_test path" : "log fallback");
}

void ui_service_show_home(const char *mode_name)
{
    const char *display_mode = mode_name ? mode_name : "Unknown";

    ESP_LOGI(TAG, "screen: Mini-CW");
    ESP_LOGI(TAG, "screen: Mode: %s", display_mode);
    ESP_LOGI(TAG, "screen: Status: %s", s_status);
    ESP_LOGI(TAG, "screen: R RX  T TX  C Call  Q QSO");
    ESP_LOGI(TAG, "screen: S Stats  M Menu  Esc Stop");

    if (s_cardputer_ready) {
        ui_cardputer_port_show_home(display_mode, s_status);
    }
}

void ui_service_show_tone_test(const ui_tone_test_view_t *view)
{
    const char *mode = (view && view->mode_name) ? view->mode_name : "Tone Test";
    const char *pattern = (view && view->last_pattern) ? view->last_pattern : "";
    const char *status = (view && view->status) ? view->status : "Ready";
    char last = (view && view->last_char) ? view->last_char : '-';
    uint8_t wpm = view ? view->wpm : 20;
    uint16_t pitch_hz = view ? view->pitch_hz : 700;

    ESP_LOGI(TAG, "screen: Mini-CW Tone Test");
    ESP_LOGI(TAG, "screen: Mode: %s", mode);
    ESP_LOGI(TAG, "screen: Press A-Z / 0-9");
    ESP_LOGI(TAG, "screen: Last: %c  %s", last, pattern);
    ESP_LOGI(TAG, "screen: WPM: %u  Pitch: %u", (unsigned)wpm, (unsigned)pitch_hz);
    ESP_LOGI(TAG, "screen: +/- WPM  [] pitch  ` stop");
    ESP_LOGI(TAG, "screen: Status: %s", status);

    if (s_cardputer_ready) {
        ui_cardputer_port_show_tone_test(view);
    }
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

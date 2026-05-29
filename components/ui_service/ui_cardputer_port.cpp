/*
 * ui_cardputer_port
 *
 * Responsibility: Initializes the Cardputer UI port and reads keyboard input
 * via the known-good mic_test M5Cardputer component.
 * Hardware ownership: display and keyboard. ui_service is the public owner;
 * this private port is the only place that initializes Cardputer UI hardware
 * or touches keyboard APIs. Normal screen composition/rendering goes through
 * ui_service -> ui_screen.
 */

#include "ui_cardputer_port.h"

#include "M5Cardputer.h"
#include "esp_log.h"

static const char *TAG = "ui_cardputer_port";

static bool s_initialized = false;
static char s_last_reported_key = '\0';

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

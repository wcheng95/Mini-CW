/*
 * ui_screen
 *
 * Responsibility: Renders the fixed 240x135 Mini-CW text layout.
 * Hardware ownership: display drawing. This file stays inside ui_service so no
 * app or trainer module talks to M5Cardputer display APIs directly.
 */

#include "ui_screen.h"

#include "M5Cardputer.h"
#include "esp_log.h"

#include <cstddef>

static const char *TAG = "ui_screen";

static bool s_initialized = false;

static void ui_screen_copy_visible(char dest[UI_COLS + 1], const char *src)
{
    std::size_t i = 0;

    if (src != nullptr) {
        for (; i < UI_COLS && src[i] != '\0'; ++i) {
            dest[i] = src[i];
        }
    }

    dest[i] = '\0';
}

static void ui_screen_select_font(void)
{
    auto &display = M5Cardputer.Display;

    /*
     * The target cell is 12x16 pixels. The current M5GFX default bitmap font is
     * 6x8, so text size 2 gives the desired 12x16 cell. Keeping this isolated
     * makes a native 12x16 font swap straightforward later.
     */
    display.setTextSize(2);
}

static void ui_screen_draw_text_row(int y, int height, const char *text, uint16_t fg, uint16_t bg)
{
    auto &display = M5Cardputer.Display;
    char clipped[UI_COLS + 1];

    ui_screen_copy_visible(clipped, text);

    display.fillRect(0, y, UI_W, height, bg);
    display.setTextColor(fg, bg);
    display.setCursor(0, y + 1);
    display.print(clipped);
}

void ui_screen_init(void)
{
    if (s_initialized) {
        return;
    }

    /*
     * Step 1 keeps hardware initialization owned by ui_service_init(), which
     * initializes the Cardputer display/keyboard port. This renderer only owns
     * fixed layout state and drawing. TODO: expose an explicit display-ready
     * query from the UI port if later startup paths render before ui_service_init().
     */
    s_initialized = true;
    ESP_LOGI(TAG, "fixed 240x135 screen renderer initialized");
}

void ui_screen_render(const mini_cw_screen_t *screen)
{
    if (screen == nullptr) {
        return;
    }

    if (!s_initialized) {
        ui_screen_init();
    }

    if (!s_initialized) {
        return;
    }

    auto &display = M5Cardputer.Display;

    display.startWrite();
    display.setRotation(1);
    display.setTextScroll(false);
    ui_screen_select_font();
    display.fillScreen(TFT_BLACK);

    ui_screen_draw_text_row(UI_TOP_Y, UI_TOP_H, screen->top, TFT_WHITE, TFT_BLACK);
    display.drawFastHLine(0, UI_SEP1_Y, UI_W, TFT_DARKGREY);

    ui_screen_draw_text_row(UI_LINE1_Y, UI_ROW_H, screen->line[0], TFT_WHITE, TFT_BLACK);
    ui_screen_draw_text_row(UI_LINE2_Y, UI_ROW_H, screen->line[1], TFT_WHITE, TFT_BLACK);
    ui_screen_draw_text_row(UI_LINE3_Y, UI_ROW_H, screen->line[2], TFT_WHITE, TFT_BLACK);
    ui_screen_draw_text_row(UI_LINE4_Y, UI_ROW_H, screen->line[3], TFT_WHITE, TFT_BLACK);
    ui_screen_draw_text_row(UI_LINE5_Y, UI_ROW_H, screen->line[4], TFT_WHITE, TFT_BLACK);

    display.drawFastHLine(0, UI_SEP2_Y, UI_W, TFT_DARKGREY);
    ui_screen_draw_text_row(UI_BOTTOM_Y, UI_BOTTOM_H, screen->bottom, TFT_WHITE, TFT_BLACK);

    display.endWrite();
}

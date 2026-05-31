/*
 * ui_screen
 *
 * Responsibility: Renders the fixed 240x135 Mini-CW text layout.
 * Hardware ownership: none. This file stays inside ui_service and calls the
 * private Cardputer port for low-level display access.
 */

#include "ui_screen.h"

#include "esp_log.h"
#include "ui_cardputer_port.h"

#include <cstddef>
#include <cstring>

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

static void ui_screen_copy_field(char *dest, std::size_t width, const char *src)
{
    std::size_t i = 0;

    if (src != nullptr) {
        for (; i < width && src[i] != '\0'; ++i) {
            dest[i] = src[i];
        }
    }

    for (; i < width; ++i) {
        dest[i] = ' ';
    }
}

static void ui_screen_format_top_row(char row[UI_COLS + 1], const mini_cw_screen_t *screen)
{
    std::size_t top_right_len = 0;
    std::size_t start;

    std::memset(row, ' ', UI_COLS);
    row[UI_COLS] = '\0';

    ui_screen_copy_field(&row[0], UI_TOP_MODE_W, screen->mode);

    top_right_len = std::strlen(screen->top_right);

    if (top_right_len > UI_COLS - UI_TOP_MODE_W - 1) {
        top_right_len = UI_COLS - UI_TOP_MODE_W - 1;
    }

    if (top_right_len == 0) {
        return;
    }

    start = UI_COLS - top_right_len;
    for (std::size_t i = 0; i < top_right_len; ++i) {
        row[start + i] = screen->top_right[i];
    }
}

static void ui_screen_draw_text_row(int y,
                                    int height,
                                    const char *text,
                                    ui_cardputer_port_color_t fg,
                                    ui_cardputer_port_color_t bg)
{
    char clipped[UI_COLS + 1];

    ui_screen_copy_visible(clipped, text);
    ui_cardputer_port_display_fill_rect(0, y, UI_W, height, bg);
    ui_cardputer_port_display_print_text(0, y + 1, clipped, fg, bg);
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

    char top[UI_COLS + 1];

    ui_screen_format_top_row(top, screen);

    ui_cardputer_port_display_begin_frame();
    ui_cardputer_port_display_fill_screen(UI_CARDPUTER_PORT_COLOR_BLACK);

    ui_screen_draw_text_row(UI_TOP_Y,
                            UI_TOP_H,
                            top,
                            UI_CARDPUTER_PORT_COLOR_WHITE,
                            UI_CARDPUTER_PORT_COLOR_BLACK);
    ui_cardputer_port_display_fill_rect(0,
                                        UI_SEP_Y,
                                        UI_W,
                                        UI_SEP_H,
                                        UI_CARDPUTER_PORT_COLOR_GREEN);

    ui_screen_draw_text_row(UI_LINE1_Y,
                            UI_ROW_H,
                            screen->line[0],
                            UI_CARDPUTER_PORT_COLOR_WHITE,
                            UI_CARDPUTER_PORT_COLOR_BLACK);
    ui_screen_draw_text_row(UI_LINE2_Y,
                            UI_ROW_H,
                            screen->line[1],
                            UI_CARDPUTER_PORT_COLOR_WHITE,
                            UI_CARDPUTER_PORT_COLOR_BLACK);
    ui_screen_draw_text_row(UI_LINE3_Y,
                            UI_ROW_H,
                            screen->line[2],
                            UI_CARDPUTER_PORT_COLOR_WHITE,
                            UI_CARDPUTER_PORT_COLOR_BLACK);
    ui_screen_draw_text_row(UI_LINE4_Y,
                            UI_ROW_H,
                            screen->line[3],
                            UI_CARDPUTER_PORT_COLOR_WHITE,
                            UI_CARDPUTER_PORT_COLOR_BLACK);
    ui_screen_draw_text_row(UI_LINE5_Y,
                            UI_ROW_H,
                            screen->line[4],
                            UI_CARDPUTER_PORT_COLOR_WHITE,
                            UI_CARDPUTER_PORT_COLOR_BLACK);

    ui_screen_draw_text_row(UI_LINE6_Y,
                            UI_ROW_H,
                            screen->line[5],
                            UI_CARDPUTER_PORT_COLOR_WHITE,
                            UI_CARDPUTER_PORT_COLOR_BLACK);

    ui_cardputer_port_display_end_frame();
}

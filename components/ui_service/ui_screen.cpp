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
static bool s_have_last_screen = false;
static mini_cw_screen_t s_last_screen;

static const int UI_LINE_Y[UI_MODE_LINES] = {
    UI_LINE1_Y,
    UI_LINE2_Y,
    UI_LINE3_Y,
    UI_LINE4_Y,
    UI_LINE5_Y,
    UI_LINE6_Y,
};

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

static void ui_screen_format_effective_top(char row[UI_COLS + 1],
                                           mini_cw_screen_color_t color[UI_COLS],
                                           const mini_cw_screen_t *screen)
{
    if (screen->top[0] != '\0') {
        ui_screen_copy_visible(row, screen->top);
        std::size_t len = std::strlen(row);
        for (std::size_t i = len; i < UI_COLS; ++i) {
            row[i] = ' ';
        }
        row[UI_COLS] = '\0';
        for (std::size_t i = 0; i < UI_COLS; ++i) {
            color[i] = screen->top_color[i] == MINI_CW_SCREEN_COLOR_DEFAULT
                           ? MINI_CW_SCREEN_COLOR_WHITE
                           : screen->top_color[i];
        }
        return;
    }

    ui_screen_format_top_row(row, screen);
    for (std::size_t i = 0; i < UI_COLS; ++i) {
        color[i] = MINI_CW_SCREEN_COLOR_WHITE;
    }
}

static ui_cardputer_port_color_t ui_screen_map_color(mini_cw_screen_color_t color)
{
    switch (color) {
    case MINI_CW_SCREEN_COLOR_GREEN:
        return UI_CARDPUTER_PORT_COLOR_GREEN;
    case MINI_CW_SCREEN_COLOR_CYAN:
        return UI_CARDPUTER_PORT_COLOR_CYAN;
    case MINI_CW_SCREEN_COLOR_WHITE:
    case MINI_CW_SCREEN_COLOR_DEFAULT:
    default:
        return UI_CARDPUTER_PORT_COLOR_WHITE;
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

static void ui_screen_draw_top_row(const char row[UI_COLS + 1],
                                   const mini_cw_screen_color_t color[UI_COLS])
{
    int run_start = 0;

    ui_cardputer_port_display_fill_rect(0, UI_TOP_Y, UI_W, UI_TOP_H, UI_CARDPUTER_PORT_COLOR_BLACK);
    while (run_start < UI_COLS) {
        int run_end = run_start + 1;
        char text[UI_COLS + 1];

        while (run_end < UI_COLS && color[run_end] == color[run_start]) {
            ++run_end;
        }

        std::memcpy(text, &row[run_start], (std::size_t)(run_end - run_start));
        text[run_end - run_start] = '\0';
        ui_cardputer_port_display_print_text(run_start * UI_FONT_W,
                                             UI_TOP_Y + 1,
                                             text,
                                             ui_screen_map_color(color[run_start]),
                                             UI_CARDPUTER_PORT_COLOR_BLACK);
        run_start = run_end;
    }
}

static bool ui_screen_line_changed(const mini_cw_screen_t *screen,
                                   const mini_cw_screen_t *last_screen,
                                   int line)
{
    if (screen->line_color[line] != last_screen->line_color[line]) {
        return true;
    }

    return std::strncmp(screen->line[line], last_screen->line[line], UI_COLS) != 0;
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
    char last_top[UI_COLS + 1];
    mini_cw_screen_color_t top_color[UI_COLS];
    mini_cw_screen_color_t last_top_color[UI_COLS];

    ui_screen_format_effective_top(top, top_color, screen);

    ui_cardputer_port_display_begin_frame();

    if (!s_have_last_screen) {
        ui_cardputer_port_display_fill_screen(UI_CARDPUTER_PORT_COLOR_BLACK);
        ui_cardputer_port_display_fill_rect(0,
                                            UI_SEP_Y,
                                            UI_W,
                                            UI_SEP_H,
                                            UI_CARDPUTER_PORT_COLOR_GREEN);
    }

    if (!s_have_last_screen) {
        last_top[0] = '\0';
        for (int i = 0; i < UI_COLS; ++i) {
            last_top_color[i] = MINI_CW_SCREEN_COLOR_DEFAULT;
        }
    } else {
        ui_screen_format_effective_top(last_top, last_top_color, &s_last_screen);
    }

    if (!s_have_last_screen || std::strcmp(top, last_top) != 0 ||
        std::memcmp(top_color, last_top_color, sizeof(top_color)) != 0) {
        ui_screen_draw_top_row(top, top_color);
    }

    for (int line = 0; line < UI_MODE_LINES; ++line) {
        if (!s_have_last_screen || ui_screen_line_changed(screen, &s_last_screen, line)) {
            ui_screen_draw_text_row(UI_LINE_Y[line],
                                    UI_ROW_H,
                                    screen->line[line],
                                    ui_screen_map_color(screen->line_color[line]),
                                    UI_CARDPUTER_PORT_COLOR_BLACK);
        }
    }

    ui_cardputer_port_display_end_frame();
    s_last_screen = *screen;
    s_have_last_screen = true;
}

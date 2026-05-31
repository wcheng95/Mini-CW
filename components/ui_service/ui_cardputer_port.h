/*
 * ui_cardputer_port
 *
 * Responsibility: Private M5Stack Cardputer UI backend used only by
 * ui_service.
 * Hardware ownership: low-level Cardputer display and keyboard access. Fixed
 * 240x135 layout is owned by ui_screen, which calls this private port for
 * hardware drawing primitives.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_CARDPUTER_PORT_EVENT_NONE = 0,
    UI_CARDPUTER_PORT_EVENT_CHAR,
    UI_CARDPUTER_PORT_EVENT_FN,
    UI_CARDPUTER_PORT_EVENT_CTRL,
    UI_CARDPUTER_PORT_EVENT_OPT,
} ui_cardputer_port_event_type_t;

typedef struct {
    ui_cardputer_port_event_type_t type;
    char ch;
} ui_cardputer_port_event_t;

typedef enum {
    UI_CARDPUTER_PORT_COLOR_BLACK = 0,
    UI_CARDPUTER_PORT_COLOR_WHITE,
    UI_CARDPUTER_PORT_COLOR_GREEN,
    UI_CARDPUTER_PORT_COLOR_CYAN,
} ui_cardputer_port_color_t;

bool ui_cardputer_port_init(void);
bool ui_cardputer_port_poll_input(ui_cardputer_port_event_t *out_event);
void ui_cardputer_port_display_begin_frame(void);
void ui_cardputer_port_display_end_frame(void);
void ui_cardputer_port_display_fill_screen(ui_cardputer_port_color_t color);
void ui_cardputer_port_display_fill_rect(int x,
                                         int y,
                                         int width,
                                         int height,
                                         ui_cardputer_port_color_t color);
void ui_cardputer_port_display_print_text(int x,
                                          int y,
                                          const char *text,
                                          ui_cardputer_port_color_t fg,
                                          ui_cardputer_port_color_t bg);
void ui_cardputer_port_display_sleep(void);

#ifdef __cplusplus
}
#endif

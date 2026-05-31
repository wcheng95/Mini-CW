/*
 * ui_screen
 *
 * Responsibility: Owns fixed 240x135 Mini-CW layout and rendering decisions.
 * Hardware ownership: none. Low-level display access stays in
 * ui_cardputer_port. This is a private ui_service helper; app_core must use
 * ui_service APIs only.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define UI_W 240
#define UI_H 135

#define UI_FONT_W 12
#define UI_FONT_H 16
#define UI_ROW_H 19
#define UI_COLS 20

#define UI_TOP_Y 0
#define UI_TOP_H 19

#define UI_SEP_Y 19
#define UI_SEP_H 2

#define UI_LINE1_Y 21
#define UI_LINE2_Y 40
#define UI_LINE3_Y 59
#define UI_LINE4_Y 78
#define UI_LINE5_Y 97
#define UI_LINE6_Y 116

#define UI_MODE_LINES 6

#define UI_TOP_MODE_W 13

typedef struct {
    char mode[UI_TOP_MODE_W + 1];
    char top_right[UI_COLS + 1];

    char line[UI_MODE_LINES][UI_COLS + 1];
} mini_cw_screen_t;

void ui_screen_init(void);
void ui_screen_render(const mini_cw_screen_t *screen);

#ifdef __cplusplus
}
#endif

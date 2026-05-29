/*
 * ui_screen
 *
 * Responsibility: Renders fixed-size Mini-CW text screens.
 * Hardware ownership: display drawing through the UI service component. This
 * is a private ui_service helper; app_core should use ui_service APIs only.
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

#define UI_SEP1_Y 19

#define UI_LINE1_Y 20
#define UI_LINE2_Y 39
#define UI_LINE3_Y 58
#define UI_LINE4_Y 77
#define UI_LINE5_Y 96

#define UI_SEP2_Y 115

#define UI_BOTTOM_Y 116
#define UI_BOTTOM_H 19

#define UI_MODE_LINES 5

typedef struct {
    char top[UI_COLS + 1];
    char line[UI_MODE_LINES][UI_COLS + 1];
    char bottom[UI_COLS + 1];
} mini_cw_screen_t;

void ui_screen_init(void);
void ui_screen_render(const mini_cw_screen_t *screen);

#ifdef __cplusplus
}
#endif

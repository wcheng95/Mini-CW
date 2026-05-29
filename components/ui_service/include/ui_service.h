/*
 * ui_service
 *
 * Responsibility: Owns screen drawing and Cardputer keyboard/input event
 * abstraction.
 * Hardware ownership: display and Cardputer keyboard/input. Other modules
 * must use ui_service APIs instead of touching display or keyboard hardware.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_INPUT_EVENT_NONE = 0,
    UI_INPUT_EVENT_MODE_RX,
    UI_INPUT_EVENT_MODE_TX,
    UI_INPUT_EVENT_MODE_CALLSIGN,
    UI_INPUT_EVENT_MODE_QSO,
    UI_INPUT_EVENT_MODE_STATS,
    UI_INPUT_EVENT_MODE_MENU,
    UI_INPUT_EVENT_CANCEL,
    UI_INPUT_EVENT_SELECT,
    UI_INPUT_EVENT_CHAR_INPUT,
    UI_INPUT_EVENT_WPM_UP,
    UI_INPUT_EVENT_WPM_DOWN,
    UI_INPUT_EVENT_PITCH_UP,
    UI_INPUT_EVENT_PITCH_DOWN,
    UI_INPUT_EVENT_FN,
} ui_input_event_type_t;

typedef struct {
    ui_input_event_type_t type;
    char key;
} ui_input_event_t;

typedef struct {
    const char *mode_name;
    char last_char;
    const char *last_pattern;
    uint8_t wpm;
    uint16_t pitch_hz;
    const char *status;
} ui_tone_test_view_t;

void ui_service_init(void);
void ui_service_show_demo_screen(void);
void ui_service_show_home(const char *mode_name);
void ui_service_show_tone_test(const ui_tone_test_view_t *view);
void ui_service_set_bottom_edit_mode(bool active);
void ui_service_set_status(const char *status);
ui_input_event_t ui_service_poll_input(void);

#ifdef __cplusplus
}
#endif

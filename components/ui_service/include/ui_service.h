/*
 * ui_service
 *
 * Responsibility: Owns UI behavior/state and exposes the public Mini-CW UI
 * API. Fixed 240x135 rendering is private to ui_screen; low-level Cardputer
 * display/keyboard access is private to ui_cardputer_port.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_INPUT_EVENT_NONE = 0,
    UI_INPUT_EVENT_CANCEL,
    UI_INPUT_EVENT_SELECT,
    UI_INPUT_EVENT_CHAR_INPUT,
    UI_INPUT_EVENT_BACKSPACE,
    UI_INPUT_EVENT_WPM_UP,
    UI_INPUT_EVENT_WPM_DOWN,
    UI_INPUT_EVENT_PITCH_UP,
    UI_INPUT_EVENT_PITCH_DOWN,
    UI_INPUT_EVENT_MODE_CHANGED,
    UI_INPUT_EVENT_LESSON_CONFIG_CHANGED,
    UI_INPUT_EVENT_SLEEP_REQUEST,
} ui_input_event_type_t;

typedef struct {
    ui_input_event_type_t type;
    char key;
} ui_input_event_t;

typedef enum {
    UI_SERVICE_MODE_PRACTICE = 0,
    UI_SERVICE_MODE_KEYER,
    UI_SERVICE_MODE_LESSONS,
    UI_SERVICE_MODE_SYSTEM,
} ui_service_mode_t;

void ui_service_init(void);
void ui_service_show_demo_screen(void);
void ui_service_refresh(void);
ui_service_mode_t ui_service_get_mode(void);
void ui_service_set_mode(ui_service_mode_t mode);
void ui_service_prepare_for_sleep(void);
ui_input_event_t ui_service_poll_input(void);

#ifdef __cplusplus
}
#endif

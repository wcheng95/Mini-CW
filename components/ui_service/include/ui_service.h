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
    UI_INPUT_EVENT_VOLUME_CHANGED,
    UI_INPUT_EVENT_KEY_IN_WPM_CHANGED,
    UI_INPUT_EVENT_KEY_IN_MODE_CHANGED,
    UI_INPUT_EVENT_LESSON_CONFIG_CHANGED,
    UI_INPUT_EVENT_WORD_CONFIG_CHANGED,
    UI_INPUT_EVENT_CALLSIGN_CONFIG_CHANGED,
    UI_INPUT_EVENT_REPLAY,
    UI_INPUT_EVENT_SLEEP_REQUEST,
} ui_input_event_type_t;

typedef enum {
    UI_SETTING_NONE = 0,
    UI_SETTING_VOLUME,
    UI_SETTING_KEY_IN_WPM,
    UI_SETTING_KEY_IN_MODE,
    UI_SETTING_LESSON,
    UI_SETTING_LESSON_DURATION,
    UI_SETTING_LESSON_CODE_WPM,
    UI_SETTING_LESSON_EFFECTIVE_WPM,
    UI_SETTING_LESSON_GROUP_LEN,
    UI_SETTING_WORD_SPEED,
    UI_SETTING_WORD_MIN_CHAR_WPM,
    UI_SETTING_WORD_LESSON,
    UI_SETTING_WORD_MAX_LEN,
    UI_SETTING_CALLSIGN_SPEED,
    UI_SETTING_CALLSIGN_MIN_CHAR_WPM,
    UI_SETTING_CALLSIGN_MAX_WPM,
} ui_setting_target_t;

typedef struct {
    ui_input_event_type_t type;
    char key;
    ui_setting_target_t setting;
    int value;
    int delta;
} ui_input_event_t;

typedef enum {
    UI_SERVICE_MODE_PRACTICE = 0,
    UI_SERVICE_MODE_KEYER,
    UI_SERVICE_MODE_LESSONS,
    UI_SERVICE_MODE_WORDS,
    UI_SERVICE_MODE_CALLSIGNS,
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

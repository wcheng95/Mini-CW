/*
 * ui_cardputer_port
 *
 * Responsibility: Private M5Stack Cardputer UI backend used only by
 * ui_service.
 * Hardware ownership: Cardputer display and keyboard stay inside ui_service.
 * This port owns Cardputer initialization and keyboard polling. Fixed-layout
 * drawing is handled by ui_screen, another private ui_service helper.
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
} ui_cardputer_port_event_type_t;

typedef struct {
    ui_cardputer_port_event_type_t type;
    char ch;
} ui_cardputer_port_event_t;

bool ui_cardputer_port_init(void);
bool ui_cardputer_port_poll_input(ui_cardputer_port_event_t *out_event);

#ifdef __cplusplus
}
#endif

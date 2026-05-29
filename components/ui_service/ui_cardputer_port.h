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

bool ui_cardputer_port_init(void);
bool ui_cardputer_port_poll_char(char *out_char);

#ifdef __cplusplus
}
#endif

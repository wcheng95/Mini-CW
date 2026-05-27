/*
 * ui_cardputer_port
 *
 * Responsibility: Private M5Stack Cardputer display and keyboard backend used
 * only by ui_service.
 * Hardware ownership: Cardputer display and keyboard. This is the only module
 * that should call M5Cardputer display/keyboard APIs.
 */

#pragma once

#include "ui_service.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool ui_cardputer_port_init(void);
void ui_cardputer_port_show_home(const char *mode_name, const char *status);
void ui_cardputer_port_show_tone_test(const ui_tone_test_view_t *view);
bool ui_cardputer_port_poll_char(char *out_char);

#ifdef __cplusplus
}
#endif

/*
 * cw_trainer_service
 *
 * Responsibility: Owns high-level CW trainer session state for RX and TX
 * practice.
 * Hardware ownership: none. This service consumes keyer events and calls other
 * services through APIs; it must not read GPIO, draw the display, drive audio
 * hardware, or access files directly.
 */

#pragma once

#include "keyer_service.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void cw_trainer_service_init(void);
void cw_trainer_start_rx_practice(void);
void cw_trainer_start_tx_practice(void);
void cw_trainer_start_tone_test(void);
void cw_trainer_stop(void);
void cw_trainer_handle_keyer_event(const keyer_event_t *event);
bool cw_trainer_handle_char_input(char ch);
void cw_trainer_adjust_wpm(int delta);
void cw_trainer_adjust_pitch(int delta_hz);

const char *cw_trainer_get_target_text(void);
const char *cw_trainer_get_copy_text(void);
char cw_trainer_get_last_char(void);
const char *cw_trainer_get_last_pattern(void);
const char *cw_trainer_get_status(void);

#ifdef __cplusplus
}
#endif

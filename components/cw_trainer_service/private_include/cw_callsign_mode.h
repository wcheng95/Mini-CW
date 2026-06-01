/*
 * Private Callsigns mode API for cw_trainer_service.
 */

#pragma once

#include "cw_trainer_service.h"

#ifdef __cplusplus
extern "C" {
#endif

void cw_callsign_mode_init(void);
const cw_callsign_config_t *cw_callsign_mode_get_config(void);
void cw_callsign_mode_set_config(const cw_callsign_config_t *config);
void cw_callsign_mode_start(void);
void cw_callsign_mode_abort(void);
bool cw_callsign_mode_append_char(char ch);
void cw_callsign_mode_backspace(void);
const cw_callsign_result_t *cw_callsign_mode_submit(void);
void cw_callsign_mode_replay(void);
const cw_callsign_view_t *cw_callsign_mode_get_view(void);
void cw_callsign_mode_load_persisted(const cw_callsign_config_t *config,
                                     const cw_callsign_result_t *result);

#ifdef __cplusplus
}
#endif

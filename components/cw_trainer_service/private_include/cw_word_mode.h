/*
 * Private Words mode API for cw_trainer_service.
 */

#pragma once

#include "cw_trainer_service.h"

#ifdef __cplusplus
extern "C" {
#endif

void cw_word_mode_init(void);
const cw_word_config_t *cw_word_mode_get_config(void);
void cw_word_mode_set_config(const cw_word_config_t *config);
void cw_word_mode_start(void);
void cw_word_mode_abort(void);
bool cw_word_mode_append_char(char ch);
void cw_word_mode_backspace(void);
const cw_word_result_t *cw_word_mode_submit(void);
void cw_word_mode_replay(void);
const cw_word_view_t *cw_word_mode_get_view(void);
void cw_word_mode_load_persisted(const cw_word_config_t *config,
                                 const cw_word_result_t *result);

#ifdef __cplusplus
}
#endif

/*
 * Private Lessons mode API for cw_trainer_service.
 */

#pragma once

#include "cw_trainer_service.h"

#ifdef __cplusplus
extern "C" {
#endif

void cw_lesson_mode_init(void);
const cw_lesson_config_t *cw_lesson_mode_get_config(void);
void cw_lesson_mode_set_config(const cw_lesson_config_t *config);
void cw_lesson_mode_start(void);
void cw_lesson_mode_abort(void);
bool cw_lesson_mode_append_char(char ch);
void cw_lesson_mode_backspace(void);
const cw_lesson_result_t *cw_lesson_mode_submit(void);
const cw_lesson_view_t *cw_lesson_mode_get_view(void);
void cw_lesson_mode_load_persisted(const cw_lesson_config_t *config,
                                   const cw_lesson_result_t *result);

#ifdef __cplusplus
}
#endif

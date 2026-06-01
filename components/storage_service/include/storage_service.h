/*
 * storage_service
 *
 * Responsibility: Owns profile, lesson, and session log persistence.
 * Hardware ownership: SD/SPIFFS/file access. Other modules must use
 * storage_service APIs instead of touching file or filesystem APIs directly.
 */

#pragma once

#include "cw_trainer_service.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void storage_service_init(void);
bool storage_profile_load(void);
bool storage_profile_save(void);
bool storage_session_log_append(const char *line);
bool storage_lesson_load(cw_lesson_config_t *config, cw_lesson_result_t *result);
bool storage_lesson_save_config(const cw_lesson_config_t *config);
bool storage_lesson_save_result(const cw_lesson_result_t *result);
bool storage_word_load(cw_word_config_t *config, cw_word_result_t *result);
bool storage_word_save_config(const cw_word_config_t *config);
bool storage_word_save_result(const cw_word_result_t *result);
bool storage_callsign_load(cw_callsign_config_t *config, cw_callsign_result_t *result);
bool storage_callsign_save_config(const cw_callsign_config_t *config);
bool storage_callsign_save_result(const cw_callsign_result_t *result);
bool storage_plaintext_load(cw_plaintext_config_t *config, cw_plaintext_result_t *result);
bool storage_plaintext_save_config(const cw_plaintext_config_t *config);
bool storage_plaintext_save_result(const cw_plaintext_result_t *result);

#ifdef __cplusplus
}
#endif

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

#ifdef __cplusplus
}
#endif

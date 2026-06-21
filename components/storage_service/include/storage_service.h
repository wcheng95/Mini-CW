/*
 * storage_service
 *
 * Responsibility: Owns profile, lesson, and session log persistence.
 * Hardware ownership: FATFS/file access and USB MSC exposure. Other modules must use
 * storage_service APIs instead of touching file or filesystem APIs directly.
 */

#pragma once

#include "cw_trainer_service.h"
#include "keyer_service.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STORAGE_SYSTEM_DATE_LEN 10U
#define STORAGE_SYSTEM_TIME_LEN 8U

void storage_service_init(void);

typedef struct {
    uint8_t volume;
    uint16_t tone_hz;
    keyer_key_in_mode_t key_in_mode;
    uint8_t key_in_wpm;
    int gps_baud;
    char date[STORAGE_SYSTEM_DATE_LEN + 1U];
    char time[STORAGE_SYSTEM_TIME_LEN + 1U];
} storage_system_config_t;

bool storage_profile_load(void);
bool storage_profile_save(void);
bool storage_session_log_append(const char *line);
bool storage_system_load_config(storage_system_config_t *config);
bool storage_system_save_config(const storage_system_config_t *config);
bool storage_keyer_load_config(keyer_config_t *config);
bool storage_keyer_save_config(const keyer_config_t *config);
bool storage_qsocalls_load(keyer_op_entry_t **entries, size_t *count);
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
bool storage_fatfs_is_mounted(void);
bool storage_usb_drive_is_enabled(void);
bool storage_usb_drive_set_enabled(bool enabled);

#ifdef __cplusplus
}
#endif

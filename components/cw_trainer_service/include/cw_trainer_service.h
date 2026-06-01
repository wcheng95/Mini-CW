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
#include <stdint.h>

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

typedef struct {
    uint8_t lesson;
    uint8_t duration_min;
    uint8_t code_wpm;
    uint8_t effective_wpm;
    uint8_t group_len;
} cw_lesson_config_t;

typedef struct {
    uint16_t target_chars;
    uint16_t copy_chars;
    uint16_t errors;
    uint8_t accuracy;
    uint32_t attempts;
    uint8_t best_accuracy;
    uint8_t last_accuracy;
} cw_lesson_result_t;

typedef enum {
    CW_LESSON_STATE_IDLE = 0,
    CW_LESSON_STATE_READY,
    CW_LESSON_STATE_COPYING,
    CW_LESSON_STATE_RESULT,
} cw_lesson_state_t;

typedef struct {
    cw_lesson_state_t state;
    cw_lesson_config_t config;
    cw_lesson_result_t result;
    const char *target_text;
    const char *copy_text;
    uint16_t target_len;
    uint16_t copy_len;
    char active_chars[42];
    char new_char;
} cw_lesson_view_t;

const cw_lesson_config_t *cw_trainer_lesson_get_config(void);
void cw_trainer_lesson_set_config(const cw_lesson_config_t *config);
void cw_trainer_lesson_start(void);
void cw_trainer_lesson_abort(void);
bool cw_trainer_lesson_append_char(char ch);
void cw_trainer_lesson_backspace(void);
const cw_lesson_result_t *cw_trainer_lesson_submit(void);
const cw_lesson_view_t *cw_trainer_lesson_get_view(void);
void cw_trainer_lesson_load_persisted(const cw_lesson_config_t *config,
                                      const cw_lesson_result_t *result);

typedef struct {
    uint8_t start_wpm;
    uint8_t min_char_wpm;
    uint8_t lesson;
    uint8_t max_word_len;
} cw_word_config_t;

typedef struct {
    uint32_t score;
    uint8_t max_wpm;
    uint8_t correct_count;
    uint8_t total_words;
    uint32_t attempts;
    uint32_t best_score;
    uint8_t best_max_wpm;
} cw_word_result_t;

typedef enum {
    CW_WORD_STATE_IDLE = 0,
    CW_WORD_STATE_READY,
    CW_WORD_STATE_COPYING,
    CW_WORD_STATE_RESULT,
} cw_word_state_t;

typedef struct {
    cw_word_state_t state;
    cw_word_config_t config;
    cw_word_result_t result;
    uint8_t current_index;
    uint8_t current_wpm;
    const char *copy_text;
    uint8_t copy_len;
    const char *last_sent_word;
    const char *last_answer;
    bool last_correct;
} cw_word_view_t;

const cw_word_config_t *cw_trainer_word_get_config(void);
void cw_trainer_word_set_config(const cw_word_config_t *config);
void cw_trainer_word_start(void);
void cw_trainer_word_abort(void);
bool cw_trainer_word_append_char(char ch);
void cw_trainer_word_backspace(void);
const cw_word_result_t *cw_trainer_word_submit(void);
void cw_trainer_word_replay(void);
const cw_word_view_t *cw_trainer_word_get_view(void);
void cw_trainer_word_load_persisted(const cw_word_config_t *config,
                                    const cw_word_result_t *result);

typedef struct {
    uint8_t start_wpm;
    uint8_t min_char_wpm;
    uint8_t max_wpm;
} cw_callsign_config_t;

typedef struct {
    uint32_t score;
    uint8_t max_wpm;
    uint8_t correct_count;
    uint8_t total_calls;
    uint32_t attempts;
    uint32_t best_score;
    uint8_t best_max_wpm;
} cw_callsign_result_t;

typedef enum {
    CW_CALLSIGN_STATE_IDLE = 0,
    CW_CALLSIGN_STATE_READY,
    CW_CALLSIGN_STATE_COPYING,
    CW_CALLSIGN_STATE_RESULT,
} cw_callsign_state_t;

typedef struct {
    cw_callsign_state_t state;
    cw_callsign_config_t config;
    cw_callsign_result_t result;
    uint8_t current_index;
    uint8_t current_wpm;
    const char *copy_text;
    uint8_t copy_len;
    const char *last_sent_call;
    const char *last_answer;
    bool last_correct;
} cw_callsign_view_t;

const cw_callsign_config_t *cw_trainer_callsign_get_config(void);
void cw_trainer_callsign_set_config(const cw_callsign_config_t *config);
void cw_trainer_callsign_start(void);
void cw_trainer_callsign_abort(void);
bool cw_trainer_callsign_append_char(char ch);
void cw_trainer_callsign_backspace(void);
const cw_callsign_result_t *cw_trainer_callsign_submit(void);
void cw_trainer_callsign_replay(void);
const cw_callsign_view_t *cw_trainer_callsign_get_view(void);
void cw_trainer_callsign_load_persisted(const cw_callsign_config_t *config,
                                        const cw_callsign_result_t *result);

typedef struct {
    uint8_t code_wpm;
    uint8_t effective_wpm;
} cw_plaintext_config_t;

typedef struct {
    uint16_t target_chars;
    uint16_t copy_chars;
    uint16_t errors;
    uint16_t accuracy_tenths;
    uint32_t attempts;
    uint16_t best_accuracy_tenths;
    uint16_t last_accuracy_tenths;
} cw_plaintext_result_t;

typedef enum {
    CW_PLAINTEXT_STATE_IDLE = 0,
    CW_PLAINTEXT_STATE_READY,
    CW_PLAINTEXT_STATE_COPYING,
    CW_PLAINTEXT_STATE_RESULT,
} cw_plaintext_state_t;

typedef struct {
    cw_plaintext_state_t state;
    cw_plaintext_config_t config;
    cw_plaintext_result_t result;
    const char *title;
    const char *source;
    const char *target_text;
    const char *copy_text;
    uint16_t target_len;
    uint16_t copy_len;
} cw_plaintext_view_t;

const cw_plaintext_config_t *cw_trainer_plaintext_get_config(void);
void cw_trainer_plaintext_set_config(const cw_plaintext_config_t *config);
void cw_trainer_plaintext_start(void);
void cw_trainer_plaintext_abort(void);
bool cw_trainer_plaintext_append_char(char ch);
void cw_trainer_plaintext_backspace(void);
const cw_plaintext_result_t *cw_trainer_plaintext_submit(void);
const cw_plaintext_view_t *cw_trainer_plaintext_get_view(void);
void cw_trainer_plaintext_load_persisted(const cw_plaintext_config_t *config,
                                         const cw_plaintext_result_t *result);

const char *cw_trainer_get_target_text(void);
const char *cw_trainer_get_copy_text(void);
char cw_trainer_get_last_char(void);
const char *cw_trainer_get_last_pattern(void);
const char *cw_trainer_get_status(void);

#ifdef __cplusplus
}
#endif

/*
 * cw_trainer_service
 *
 * Responsibility: Owns high-level CW trainer session coordination and shared
 * trainer helpers. Mode-specific Lessons, Words, Callsigns, and PlainText
 * state lives in private mode implementation files.
 * Hardware ownership: none. This service consumes input, prepares training
 * text, requests playback through audio_service, and exposes view state to UI.
 */

#include "cw_trainer_service.h"

#include "cw_callsign_mode.h"
#include "cw_lesson_mode.h"
#include "cw_plaintext_mode.h"
#include "cw_trainer_internal.h"
#include "cw_word_mode.h"

#include "audio_service.h"
#include "esp_log.h"
#ifndef CW_TRAINER_LESSON_DETERMINISTIC_RANDOM
#include "esp_random.h"
#endif

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "cw_trainer_service";

#define CW_TRAINER_TEXT_COMPARE_MAX 1024U

const char CW_TRAINER_KOCH_CHARS[] =
    "KMURESNAPTLWI.JZ=FOY,VG5/Q92H38B?47C1D60X";
static char s_target_text[64];
static char s_copy_text[64];
static const char *s_last_pattern;
static char s_last_char;
static const char *s_status = "Ready";
static bool s_rx_active;
static bool s_tx_active;
static bool s_tone_test_active;
uint8_t cw_trainer_clamp_u8(uint8_t value, uint8_t min_value, uint8_t max_value)
{
    if (value < min_value) {
        return min_value;
    }

    if (value > max_value) {
        return max_value;
    }

    return value;
}

uint32_t cw_trainer_rand_u32(void)
{
#ifdef CW_TRAINER_LESSON_DETERMINISTIC_RANDOM
    uint32_t value = ((uint32_t)rand() & 0xffffU) << 16;
    value |= ((uint32_t)rand() & 0xffffU);
    return value;
#else
    return esp_random();
#endif
}

void cw_trainer_seed_random(void)
{
#ifdef CW_TRAINER_LESSON_DETERMINISTIC_RANDOM
    srand(1U);
#endif
}

uint16_t cw_trainer_levenshtein(const char *a, const char *b, uint16_t max_len)
{
    static uint16_t prev[CW_TRAINER_TEXT_COMPARE_MAX + 1U];
    static uint16_t curr[CW_TRAINER_TEXT_COMPARE_MAX + 1U];
    uint16_t len_a = (uint16_t)strlen(a);
    uint16_t len_b = (uint16_t)strlen(b);
    uint16_t i;
    uint16_t j;

    if (max_len > CW_TRAINER_TEXT_COMPARE_MAX) {
        max_len = CW_TRAINER_TEXT_COMPARE_MAX;
    }

    if (len_a > max_len) {
        len_a = max_len;
    }
    if (len_b > max_len) {
        len_b = max_len;
    }

    for (j = 0; j <= len_b; ++j) {
        prev[j] = j;
    }

    for (i = 1; i <= len_a; ++i) {
        curr[0] = i;

        for (j = 1; j <= len_b; ++j) {
            uint16_t deletion = (uint16_t)(prev[j] + 1U);
            uint16_t insertion = (uint16_t)(curr[j - 1U] + 1U);
            uint16_t substitution =
                (uint16_t)(prev[j - 1U] + (a[i - 1U] == b[j - 1U] ? 0U : 1U));
            uint16_t best = deletion < insertion ? deletion : insertion;
            curr[j] = best < substitution ? best : substitution;
        }

        memcpy(prev, curr, (len_b + 1U) * sizeof(prev[0]));
    }

    return prev[len_b];
}

void cw_trainer_service_init(void)
{
    snprintf(s_target_text, sizeof(s_target_text), "%s", "CQ MINI CW");
    s_copy_text[0] = '\0';
    s_last_pattern = "";
    s_last_char = '\0';
    s_rx_active = false;
    s_tx_active = false;
    s_tone_test_active = false;

    cw_trainer_seed_random();
    cw_lesson_mode_init();
    cw_word_mode_init();
    cw_callsign_mode_init();
    cw_plaintext_mode_init();

    ESP_LOGI(TAG, "initialized trainer service");
}
void cw_trainer_start_rx_practice(void)
{
    s_rx_active = true;
    s_tx_active = false;
    s_tone_test_active = false;
    s_copy_text[0] = '\0';

    ESP_LOGI(TAG, "start RX practice stub; target='%s'", s_target_text);
}

void cw_trainer_start_tx_practice(void)
{
    s_rx_active = false;
    s_tx_active = true;
    s_tone_test_active = false;
    s_copy_text[0] = '\0';

    ESP_LOGI(TAG, "start TX practice stub; target='%s'", s_target_text);
}

void cw_trainer_start_tone_test(void)
{
    s_rx_active = false;
    s_tx_active = false;
    s_tone_test_active = true;
    s_status = "Ready";

    ESP_LOGI(TAG, "start tone test");
}

void cw_trainer_stop(void)
{
    if (s_rx_active || s_tx_active || s_tone_test_active) {
        ESP_LOGI(TAG, "stop practice");
    }

    s_rx_active = false;
    s_tx_active = false;
    s_tone_test_active = false;
    s_status = "Stopped";
}

void cw_trainer_handle_keyer_event(const keyer_event_t *event)
{
    if (event == NULL || event->type == KEYER_EVENT_NONE) {
        return;
    }

    ESP_LOGI(TAG,
             "keyer event: type=%d char='%c' duration=%u ms",
             event->type,
             event->decoded_char ? event->decoded_char : ' ',
             (unsigned)event->duration_ms);
}

bool cw_trainer_handle_char_input(char ch)
{
    char normalized = (char)toupper((unsigned char)ch);
    const char *pattern = audio_service_get_cw_pattern(normalized);

    if (pattern == NULL) {
        s_status = "Unsupported";
        ESP_LOGW(TAG, "unsupported character input: '%c'", ch);
        return false;
    }

    s_last_char = normalized;
    s_last_pattern = pattern;
    s_status = "Queued";

    ESP_LOGI(TAG, "character input: %c %s", s_last_char, s_last_pattern);
    audio_service_play_cw_char(normalized);
    return true;
}

void cw_trainer_adjust_wpm(int delta)
{
    int next = (int)audio_service_get_cw_wpm() + delta;
    if (next < 0) {
        next = 0;
    }

    audio_service_set_cw_wpm((uint8_t)next);
    s_status = "WPM updated";
}

void cw_trainer_adjust_pitch(int delta_hz)
{
    int next = (int)audio_service_get_tone_hz() + delta_hz;
    if (next < 0) {
        next = 0;
    }

    audio_service_set_tone_hz((uint16_t)next);
    s_status = "Pitch updated";
}
const cw_lesson_config_t *cw_trainer_lesson_get_config(void)
{
    return cw_lesson_mode_get_config();
}

void cw_trainer_lesson_set_config(const cw_lesson_config_t *config)
{
    cw_lesson_mode_set_config(config);
}

void cw_trainer_lesson_start(void)
{
    cw_lesson_mode_start();
}

void cw_trainer_lesson_abort(void)
{
    cw_lesson_mode_abort();
}

bool cw_trainer_lesson_append_char(char ch)
{
    return cw_lesson_mode_append_char(ch);
}

void cw_trainer_lesson_backspace(void)
{
    cw_lesson_mode_backspace();
}

const cw_lesson_result_t *cw_trainer_lesson_submit(void)
{
    return cw_lesson_mode_submit();
}

const cw_lesson_view_t *cw_trainer_lesson_get_view(void)
{
    return cw_lesson_mode_get_view();
}

void cw_trainer_lesson_load_persisted(const cw_lesson_config_t *config,
                                      const cw_lesson_result_t *result)
{
    cw_lesson_mode_load_persisted(config, result);
}

const cw_word_config_t *cw_trainer_word_get_config(void)
{
    return cw_word_mode_get_config();
}

void cw_trainer_word_set_config(const cw_word_config_t *config)
{
    cw_word_mode_set_config(config);
}

void cw_trainer_word_start(void)
{
    cw_word_mode_start();
}

void cw_trainer_word_abort(void)
{
    cw_word_mode_abort();
}

bool cw_trainer_word_append_char(char ch)
{
    return cw_word_mode_append_char(ch);
}

void cw_trainer_word_backspace(void)
{
    cw_word_mode_backspace();
}

const cw_word_result_t *cw_trainer_word_submit(void)
{
    return cw_word_mode_submit();
}

void cw_trainer_word_replay(void)
{
    cw_word_mode_replay();
}

const cw_word_view_t *cw_trainer_word_get_view(void)
{
    return cw_word_mode_get_view();
}

void cw_trainer_word_load_persisted(const cw_word_config_t *config,
                                    const cw_word_result_t *result)
{
    cw_word_mode_load_persisted(config, result);
}

const cw_callsign_config_t *cw_trainer_callsign_get_config(void)
{
    return cw_callsign_mode_get_config();
}

void cw_trainer_callsign_set_config(const cw_callsign_config_t *config)
{
    cw_callsign_mode_set_config(config);
}

void cw_trainer_callsign_start(void)
{
    cw_callsign_mode_start();
}

void cw_trainer_callsign_abort(void)
{
    cw_callsign_mode_abort();
}

bool cw_trainer_callsign_append_char(char ch)
{
    return cw_callsign_mode_append_char(ch);
}

void cw_trainer_callsign_backspace(void)
{
    cw_callsign_mode_backspace();
}

const cw_callsign_result_t *cw_trainer_callsign_submit(void)
{
    return cw_callsign_mode_submit();
}

void cw_trainer_callsign_replay(void)
{
    cw_callsign_mode_replay();
}

const cw_callsign_view_t *cw_trainer_callsign_get_view(void)
{
    return cw_callsign_mode_get_view();
}

void cw_trainer_callsign_load_persisted(const cw_callsign_config_t *config,
                                        const cw_callsign_result_t *result)
{
    cw_callsign_mode_load_persisted(config, result);
}

const cw_plaintext_config_t *cw_trainer_plaintext_get_config(void)
{
    return cw_plaintext_mode_get_config();
}

void cw_trainer_plaintext_set_config(const cw_plaintext_config_t *config)
{
    cw_plaintext_mode_set_config(config);
}

void cw_trainer_plaintext_start(void)
{
    cw_plaintext_mode_start();
}

void cw_trainer_plaintext_abort(void)
{
    cw_plaintext_mode_abort();
}

bool cw_trainer_plaintext_append_char(char ch)
{
    return cw_plaintext_mode_append_char(ch);
}

void cw_trainer_plaintext_backspace(void)
{
    cw_plaintext_mode_backspace();
}

const cw_plaintext_result_t *cw_trainer_plaintext_submit(void)
{
    return cw_plaintext_mode_submit();
}

const cw_plaintext_view_t *cw_trainer_plaintext_get_view(void)
{
    return cw_plaintext_mode_get_view();
}

void cw_trainer_plaintext_load_persisted(const cw_plaintext_config_t *config,
                                         const cw_plaintext_result_t *result)
{
    cw_plaintext_mode_load_persisted(config, result);
}
const char *cw_trainer_get_target_text(void)
{
    return s_target_text;
}

const char *cw_trainer_get_copy_text(void)
{
    return s_copy_text;
}

char cw_trainer_get_last_char(void)
{
    return s_last_char;
}

const char *cw_trainer_get_last_pattern(void)
{
    return s_last_pattern;
}

const char *cw_trainer_get_status(void)
{
    return s_status;
}

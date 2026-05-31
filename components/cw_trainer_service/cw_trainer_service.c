/*
 * cw_trainer_service
 *
 * Responsibility: Owns high-level CW trainer session state for RX, TX, and
 * embedded Koch lesson practice.
 * Hardware ownership: none. This service consumes input, prepares training
 * text, requests playback through audio_service, and exposes view state to UI.
 */

#include "cw_trainer_service.h"

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

#define CW_LESSON_MIN 1U
#define CW_LESSON_MAX 40U
#define CW_LESSON_DURATION_MIN 1U
#define CW_LESSON_DURATION_MAX 5U
#define CW_LESSON_WPM_MIN 5U
#define CW_LESSON_WPM_MAX 40U
#define CW_LESSON_TARGET_MAX 1024U
#define CW_LESSON_COPY_MAX 1024U

static const char KOCH_CHARS[] =
    "KMURESNAPTLWI.JZ=FOY,VG5/Q92H38B?47C1D60X";

static char s_target_text[64];
static char s_copy_text[64];
static const char *s_last_pattern;
static char s_last_char;
static const char *s_status = "Ready";
static bool s_rx_active;
static bool s_tx_active;
static bool s_tone_test_active;

static cw_lesson_config_t s_lesson_config = {
    .lesson = 1,
    .duration_min = 1,
    .code_wpm = 20,
    .effective_wpm = 12,
    .group_len = 0,
};

static cw_lesson_result_t s_lesson_result = {
    .target_chars = 0,
    .copy_chars = 0,
    .errors = 0,
    .accuracy = 0,
    .attempts = 0,
    .best_accuracy = 0,
    .last_accuracy = 0,
};

static cw_lesson_view_t s_lesson_view;
static char s_lesson_target[CW_LESSON_TARGET_MAX + 1U];
static char s_lesson_copy[CW_LESSON_COPY_MAX + 1U];
static uint16_t s_lesson_target_len;
static uint16_t s_lesson_copy_len;

static uint8_t clamp_u8(uint8_t value, uint8_t min_value, uint8_t max_value)
{
    if (value < min_value) {
        return min_value;
    }

    if (value > max_value) {
        return max_value;
    }

    return value;
}

static void cw_lesson_normalize_config(cw_lesson_config_t *config)
{
    if (config == NULL) {
        return;
    }

    config->lesson = clamp_u8(config->lesson, CW_LESSON_MIN, CW_LESSON_MAX);
    config->duration_min =
        clamp_u8(config->duration_min, CW_LESSON_DURATION_MIN, CW_LESSON_DURATION_MAX);
    config->code_wpm = clamp_u8(config->code_wpm, CW_LESSON_WPM_MIN, CW_LESSON_WPM_MAX);
    config->effective_wpm =
        clamp_u8(config->effective_wpm, CW_LESSON_WPM_MIN, CW_LESSON_WPM_MAX);

    if (config->effective_wpm > config->code_wpm) {
        config->effective_wpm = config->code_wpm;
    }

    if (config->group_len != 0U) {
        config->group_len = clamp_u8(config->group_len, 2U, 7U);
    }
}

static bool cw_lesson_config_equal(const cw_lesson_config_t *a, const cw_lesson_config_t *b)
{
    return a != NULL && b != NULL && a->lesson == b->lesson &&
           a->duration_min == b->duration_min && a->code_wpm == b->code_wpm &&
           a->effective_wpm == b->effective_wpm && a->group_len == b->group_len;
}

static uint32_t cw_lesson_rand_u32(void)
{
#ifdef CW_TRAINER_LESSON_DETERMINISTIC_RANDOM
    uint32_t value = ((uint32_t)rand() & 0xffffU) << 16;
    value |= ((uint32_t)rand() & 0xffffU);
    return value;
#else
    return esp_random();
#endif
}

static void cw_lesson_seed_random(void)
{
#ifdef CW_TRAINER_LESSON_DETERMINISTIC_RANDOM
    srand(1U);
#else
    (void)esp_random();
#endif
}

static uint8_t cw_lesson_random_range(uint8_t min_value, uint8_t max_value)
{
    uint8_t span;

    if (max_value <= min_value) {
        return min_value;
    }

    span = (uint8_t)(max_value - min_value + 1U);
    return (uint8_t)(min_value + (cw_lesson_rand_u32() % span));
}

static uint8_t cw_lesson_random_group_len(void)
{
    uint8_t draw = cw_lesson_random_range(1U, 18U);

    if (draw < 3U) {
        return 2U;
    }

    if (draw < 6U) {
        return 3U;
    }

    if (draw < 10U) {
        return 4U;
    }

    if (draw < 14U) {
        return 5U;
    }

    if (draw < 17U) {
        return 6U;
    }

    return 7U;
}

static uint8_t cw_lesson_active_count(uint8_t lesson)
{
    lesson = clamp_u8(lesson, CW_LESSON_MIN, CW_LESSON_MAX);
    return (uint8_t)(lesson + 1U);
}

static uint8_t cw_lesson_pick_char_index(uint8_t active_count)
{
    if (active_count <= 1U) {
        return 0U;
    }

    if (active_count <= 10U) {
        return cw_lesson_random_range(0U, (uint8_t)(active_count - 1U));
    }

    /*
     * Bias toward newer lesson characters so recently introduced symbols appear
     * more often. This approximates LCWO-style learning behavior without copying
     * LCWO source internals.
     */
    uint8_t a = cw_lesson_random_range(0U, (uint8_t)(active_count - 1U));
    uint8_t b = cw_lesson_random_range(0U, (uint8_t)(active_count - 1U));
    return a > b ? a : b;
}

static bool cw_lesson_char_supported(char ch)
{
    return audio_service_get_cw_pattern(ch) != NULL;
}

static void cw_lesson_update_view(void)
{
    cw_lesson_state_t state = s_lesson_view.state;
    uint8_t active_count = cw_lesson_active_count(s_lesson_config.lesson);
    uint8_t i;

    memset(&s_lesson_view, 0, sizeof(s_lesson_view));
    s_lesson_view.state = state == CW_LESSON_STATE_IDLE ? CW_LESSON_STATE_READY : state;
    s_lesson_view.config = s_lesson_config;
    s_lesson_view.result = s_lesson_result;
    s_lesson_view.target_text = s_lesson_target;
    s_lesson_view.copy_text = s_lesson_copy;
    s_lesson_view.target_len = s_lesson_target_len;
    s_lesson_view.copy_len = s_lesson_copy_len;
    s_lesson_view.new_char = KOCH_CHARS[active_count - 1U];

    for (i = 0; i < active_count && i < sizeof(s_lesson_view.active_chars) - 1U; ++i) {
        s_lesson_view.active_chars[i] = KOCH_CHARS[i];
    }
    s_lesson_view.active_chars[i] = '\0';
}

static void cw_lesson_set_state(cw_lesson_state_t state)
{
    s_lesson_view.state = state;
    cw_lesson_update_view();
}

static void cw_lesson_clear_copy(void)
{
    s_lesson_copy[0] = '\0';
    s_lesson_copy_len = 0;
}

static void cw_lesson_clear_session_text(void)
{
    s_lesson_target[0] = '\0';
    s_lesson_target_len = 0;
    cw_lesson_clear_copy();
}

static void cw_lesson_reset_session(bool stop_playback)
{
    if (stop_playback) {
        audio_service_stop_all();
    }

    cw_lesson_clear_session_text();
    cw_lesson_set_state(CW_LESSON_STATE_READY);
}

static uint16_t cw_lesson_normalized_len(const char *text)
{
    uint16_t count = 0;
    const char *cursor;

    if (text == NULL) {
        return 0;
    }

    for (cursor = text; *cursor != '\0'; ++cursor) {
        if (!isspace((unsigned char)*cursor)) {
            ++count;
        }
    }

    return count;
}

static uint16_t cw_lesson_levenshtein(const char *a, const char *b)
{
    static uint16_t prev[CW_LESSON_TARGET_MAX + 1U];
    static uint16_t curr[CW_LESSON_TARGET_MAX + 1U];
    uint16_t len_a = (uint16_t)strlen(a);
    uint16_t len_b = (uint16_t)strlen(b);
    uint16_t i;
    uint16_t j;

    if (len_a > CW_LESSON_TARGET_MAX) {
        len_a = CW_LESSON_TARGET_MAX;
    }
    if (len_b > CW_LESSON_TARGET_MAX) {
        len_b = CW_LESSON_TARGET_MAX;
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

static void cw_lesson_compact_text(const char *in, char *out, size_t out_size)
{
    bool pending_space = false;
    size_t len = 0;

    if (out == NULL || out_size == 0U) {
        return;
    }

    out[0] = '\0';

    if (in == NULL) {
        return;
    }

    while (*in != '\0' && len + 1U < out_size) {
        char ch = (char)toupper((unsigned char)*in);

        if (ch == ';') {
            ch = '?';
        }

        if (isspace((unsigned char)ch)) {
            pending_space = len > 0U;
        } else if (cw_lesson_char_supported(ch)) {
            if (pending_space && len + 1U < out_size) {
                out[len++] = ' ';
            }
            out[len++] = ch;
            pending_space = false;
        }

        ++in;
    }

    out[len] = '\0';
}

static void cw_lesson_generate_target(void)
{
    uint8_t active_count = cw_lesson_active_count(s_lesson_config.lesson);
    uint16_t desired_chars =
        (uint16_t)s_lesson_config.duration_min * (uint16_t)s_lesson_config.effective_wpm * 5U;
    uint16_t generated_chars = 0;
    size_t out_len = 0;

    s_lesson_target[0] = '\0';
    s_lesson_target_len = 0;

    while (generated_chars < desired_chars && out_len + 8U < sizeof(s_lesson_target)) {
        uint8_t group_len =
            s_lesson_config.group_len == 0U ? cw_lesson_random_group_len()
                                            : s_lesson_config.group_len;

        if (out_len > 0U) {
            s_lesson_target[out_len++] = ' ';
        }

        for (uint8_t i = 0; i < group_len && out_len + 1U < sizeof(s_lesson_target); ++i) {
            uint8_t idx = cw_lesson_pick_char_index(active_count);
            s_lesson_target[out_len++] = KOCH_CHARS[idx];
            ++generated_chars;
        }

        s_lesson_target[out_len] = '\0';
    }

    s_lesson_target_len = (uint16_t)strlen(s_lesson_target);
    ESP_LOGI(TAG,
             "lesson generated: lesson=%u active=%s chars=%u text_len=%u",
             (unsigned)s_lesson_config.lesson,
             s_lesson_view.active_chars,
             (unsigned)cw_lesson_normalized_len(s_lesson_target),
             (unsigned)s_lesson_target_len);
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
    cw_lesson_clear_session_text();
    cw_lesson_seed_random();
    cw_lesson_set_state(CW_LESSON_STATE_READY);

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
    return &s_lesson_config;
}

void cw_trainer_lesson_set_config(const cw_lesson_config_t *config)
{
    cw_lesson_config_t next_config;
    bool changed;
    bool was_copying;

    if (config == NULL) {
        return;
    }

    next_config = *config;
    cw_lesson_normalize_config(&next_config);
    changed = !cw_lesson_config_equal(&next_config, &s_lesson_config);
    was_copying = s_lesson_view.state == CW_LESSON_STATE_COPYING;
    s_lesson_config = next_config;

    if (changed) {
        cw_lesson_reset_session(was_copying);
    } else {
        cw_lesson_update_view();
    }

    ESP_LOGI(TAG,
             "lesson config: lesson=%u duration=%u code=%u eff=%u group=%u",
             (unsigned)s_lesson_config.lesson,
             (unsigned)s_lesson_config.duration_min,
             (unsigned)s_lesson_config.code_wpm,
             (unsigned)s_lesson_config.effective_wpm,
             (unsigned)s_lesson_config.group_len);
}

void cw_trainer_lesson_abort(void)
{
    cw_lesson_reset_session(true);
    ESP_LOGI(TAG, "lesson aborted");
}

void cw_trainer_lesson_start(void)
{
    cw_lesson_clear_copy();
    cw_lesson_generate_target();

    audio_service_set_cw_wpm(s_lesson_config.code_wpm);
    audio_service_set_cw_farnsworth_wpm(s_lesson_config.effective_wpm);
    audio_service_play_cw_text(s_lesson_target);
    cw_lesson_set_state(CW_LESSON_STATE_COPYING);
}

bool cw_trainer_lesson_append_char(char ch)
{
    char normalized = (char)toupper((unsigned char)ch);

    if (s_lesson_view.state != CW_LESSON_STATE_COPYING) {
        return false;
    }

    if (normalized == ';') {
        normalized = '?';
    }

    if (isspace((unsigned char)normalized)) {
        normalized = ' ';
    } else if (!cw_lesson_char_supported(normalized)) {
        return false;
    }

    if (s_lesson_copy_len >= CW_LESSON_COPY_MAX) {
        return false;
    }

    s_lesson_copy[s_lesson_copy_len++] = normalized;
    s_lesson_copy[s_lesson_copy_len] = '\0';
    cw_lesson_update_view();
    return true;
}

void cw_trainer_lesson_backspace(void)
{
    if (s_lesson_view.state != CW_LESSON_STATE_COPYING || s_lesson_copy_len == 0U) {
        return;
    }

    --s_lesson_copy_len;
    s_lesson_copy[s_lesson_copy_len] = '\0';
    cw_lesson_update_view();
}

const cw_lesson_result_t *cw_trainer_lesson_submit(void)
{
    char target[CW_LESSON_TARGET_MAX + 1U];
    char copy[CW_LESSON_COPY_MAX + 1U];
    uint16_t target_chars;
    uint16_t distance;
    uint16_t accuracy = 0;

    cw_lesson_compact_text(s_lesson_target, target, sizeof(target));
    cw_lesson_compact_text(s_lesson_copy, copy, sizeof(copy));

    target_chars = cw_lesson_normalized_len(target);
    distance = cw_lesson_levenshtein(target, copy);

    if (target_chars > 0U) {
        uint32_t bounded_distance = distance > target_chars ? target_chars : distance;
        accuracy = (uint16_t)(100U - ((bounded_distance * 100U) / target_chars));
    }

    s_lesson_result.target_chars = target_chars;
    s_lesson_result.copy_chars = cw_lesson_normalized_len(copy);
    s_lesson_result.errors = distance;
    s_lesson_result.accuracy = (uint8_t)accuracy;
    s_lesson_result.last_accuracy = (uint8_t)accuracy;
    ++s_lesson_result.attempts;

    if (s_lesson_result.accuracy > s_lesson_result.best_accuracy) {
        s_lesson_result.best_accuracy = s_lesson_result.accuracy;
    }

    ESP_LOGI(TAG,
             "lesson result: accuracy=%u errors=%u target=%u copy=%u attempts=%lu best=%u",
             (unsigned)s_lesson_result.accuracy,
             (unsigned)s_lesson_result.errors,
             (unsigned)s_lesson_result.target_chars,
             (unsigned)s_lesson_result.copy_chars,
             (unsigned long)s_lesson_result.attempts,
             (unsigned)s_lesson_result.best_accuracy);

    cw_lesson_set_state(CW_LESSON_STATE_RESULT);
    return &s_lesson_result;
}

const cw_lesson_view_t *cw_trainer_lesson_get_view(void)
{
    cw_lesson_update_view();
    return &s_lesson_view;
}

void cw_trainer_lesson_load_persisted(const cw_lesson_config_t *config,
                                      const cw_lesson_result_t *result)
{
    if (config != NULL) {
        s_lesson_config = *config;
        cw_lesson_normalize_config(&s_lesson_config);
    }

    if (result != NULL) {
        s_lesson_result = *result;
    }

    cw_lesson_reset_session(false);
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

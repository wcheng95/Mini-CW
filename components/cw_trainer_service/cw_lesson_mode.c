/*
 * Mode implementation for cw_trainer_service.
 * Private to the cw_trainer_service component.
 */

#include "cw_lesson_mode.h"
#include "cw_trainer_internal.h"

#include "audio_service.h"
#include "esp_log.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "cw_lesson_mode";
#define CW_LESSON_MIN 1U
#define CW_LESSON_MAX 40U
#define CW_LESSON_DURATION_MIN 1U
#define CW_LESSON_DURATION_MAX 5U
#define CW_LESSON_WPM_MIN 5U
#define CW_LESSON_WPM_MAX 40U
#define CW_LESSON_TARGET_MAX 1024U
#define CW_LESSON_COPY_MAX 1024U

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

static void cw_lesson_normalize_config(cw_lesson_config_t *config)
{
    if (config == NULL) {
        return;
    }

    config->lesson = cw_trainer_clamp_u8(config->lesson, CW_LESSON_MIN, CW_LESSON_MAX);
    config->duration_min =
        cw_trainer_clamp_u8(config->duration_min, CW_LESSON_DURATION_MIN, CW_LESSON_DURATION_MAX);
    config->code_wpm = cw_trainer_clamp_u8(config->code_wpm, CW_LESSON_WPM_MIN, CW_LESSON_WPM_MAX);
    config->effective_wpm =
        cw_trainer_clamp_u8(config->effective_wpm, CW_LESSON_WPM_MIN, CW_LESSON_WPM_MAX);

    if (config->effective_wpm > config->code_wpm) {
        config->effective_wpm = config->code_wpm;
    }

    if (config->group_len != 0U) {
        config->group_len = cw_trainer_clamp_u8(config->group_len, 2U, 7U);
    }
}

static bool cw_lesson_config_equal(const cw_lesson_config_t *a, const cw_lesson_config_t *b)
{
    return a != NULL && b != NULL && a->lesson == b->lesson &&
           a->duration_min == b->duration_min && a->code_wpm == b->code_wpm &&
           a->effective_wpm == b->effective_wpm && a->group_len == b->group_len;
}

static uint8_t cw_lesson_random_range(uint8_t min_value, uint8_t max_value)
{
    uint8_t span;

    if (max_value <= min_value) {
        return min_value;
    }

    span = (uint8_t)(max_value - min_value + 1U);
    return (uint8_t)(min_value + (cw_trainer_rand_u32() % span));
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
    lesson = cw_trainer_clamp_u8(lesson, CW_LESSON_MIN, CW_LESSON_MAX);
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
    s_lesson_view.new_char = CW_TRAINER_KOCH_CHARS[active_count - 1U];

    for (i = 0; i < active_count && i < sizeof(s_lesson_view.active_chars) - 1U; ++i) {
        s_lesson_view.active_chars[i] = CW_TRAINER_KOCH_CHARS[i];
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
            s_lesson_target[out_len++] = CW_TRAINER_KOCH_CHARS[idx];
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

void cw_lesson_mode_init(void)
{
    cw_lesson_clear_session_text();
    cw_lesson_set_state(CW_LESSON_STATE_READY);
}

const cw_lesson_config_t *cw_lesson_mode_get_config(void)
{
    return &s_lesson_config;
}

void cw_lesson_mode_set_config(const cw_lesson_config_t *config)
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

void cw_lesson_mode_abort(void)
{
    cw_lesson_reset_session(true);
    ESP_LOGI(TAG, "lesson aborted");
}

void cw_lesson_mode_start(void)
{
    cw_lesson_clear_copy();
    cw_lesson_generate_target();

    audio_service_set_cw_wpm(s_lesson_config.code_wpm);
    audio_service_set_cw_farnsworth_wpm(s_lesson_config.effective_wpm);
    audio_service_play_cw_text(s_lesson_target);
    cw_lesson_set_state(CW_LESSON_STATE_COPYING);
}

bool cw_lesson_mode_append_char(char ch)
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

void cw_lesson_mode_backspace(void)
{
    if (s_lesson_view.state != CW_LESSON_STATE_COPYING || s_lesson_copy_len == 0U) {
        return;
    }

    --s_lesson_copy_len;
    s_lesson_copy[s_lesson_copy_len] = '\0';
    cw_lesson_update_view();
}

const cw_lesson_result_t *cw_lesson_mode_submit(void)
{
    char target[CW_LESSON_TARGET_MAX + 1U];
    char copy[CW_LESSON_COPY_MAX + 1U];
    uint16_t target_chars;
    uint16_t distance;
    uint16_t accuracy = 0;

    cw_lesson_compact_text(s_lesson_target, target, sizeof(target));
    cw_lesson_compact_text(s_lesson_copy, copy, sizeof(copy));

    target_chars = cw_lesson_normalized_len(target);
    distance = cw_trainer_levenshtein(target, copy, CW_LESSON_TARGET_MAX);

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

const cw_lesson_view_t *cw_lesson_mode_get_view(void)
{
    cw_lesson_update_view();
    return &s_lesson_view;
}

void cw_lesson_mode_load_persisted(const cw_lesson_config_t *config,
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

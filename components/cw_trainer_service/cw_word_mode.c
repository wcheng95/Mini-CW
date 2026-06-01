/*
 * Mode implementation for cw_trainer_service.
 * Private to the cw_trainer_service component.
 */

#include "cw_word_mode.h"
#include "cw_trainer_internal.h"

#include "audio_service.h"
#include "esp_log.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "cw_word_mode";
#define CW_WORD_ATTEMPT_WORDS 25U
#define CW_WORD_WPM_MIN 5U
#define CW_WORD_WPM_MAX 40U
#define CW_WORD_LESSON_MIN 9U
#define CW_WORD_LESSON_MAX 40U
#define CW_WORD_MAX_LEN_MIN 2U
#define CW_WORD_MAX_LEN_MAX 15U
#define CW_WORD_COPY_MAX 32U

static const char *const CW_WORD_BANK[] = {
    "an",        "am",       "as",       "at",       "me",        "men",
    "ten",       "net",      "sun",      "run",      "ran",       "man",
    "map",       "mat",      "pan",      "pen",      "pet",       "set",
    "sea",       "ear",      "tea",      "team",     "meat",      "mean",
    "near",      "neat",     "seat",     "star",     "start",     "stream",
    "street",    "test",     "treat",    "trust",    "turn",      "must",
    "master",    "market",   "summer",   "runner",   "keeper",    "part",
    "parts",     "past",     "same",     "name",     "names",     "tune",
    "tuner",     "antenna",  "listen",   "line",     "wire",      "radio",
    "signal",    "audio",    "tone",     "dit",      "dah",       "copy",
    "code",      "word",     "words",    "speed",    "pitch",     "field",
    "power",     "meter",    "station",  "contact",  "serial",    "report",
    "operator",  "portable", "summit",   "park",     "trail",     "river",
    "county",    "state",    "north",    "south",    "east",      "west",
    "local",     "remote",   "mobile",   "simple",   "practice",  "lesson",
    "training",  "message",  "receive",  "answer",   "correct",   "wrong",
    "repeat",    "score",    "faster",   "slower",   "finish",    "ready",
    "clear",     "change",   "random",   "letter",   "number",    "friend",
    "weather",   "morning",  "evening",  "thanks",   "again",     "about",
    "above",     "after",    "alone",    "along",    "another",   "around",
    "because",   "before",   "between",  "during",   "example",   "family",
    "future",    "general",  "history",  "important","measure",   "question",
    "support",   "through",  "without",
};

static cw_word_config_t s_word_config = {
    .start_wpm = 20,
    .min_char_wpm = 10,
    .lesson = 40,
    .max_word_len = 15,
};

static cw_word_result_t s_word_result = {
    .score = 0,
    .max_wpm = 0,
    .correct_count = 0,
    .total_words = CW_WORD_ATTEMPT_WORDS,
    .attempts = 0,
    .best_score = 0,
    .best_max_wpm = 0,
};

static cw_word_view_t s_word_view;
static const char *s_word_attempt[CW_WORD_ATTEMPT_WORDS];
static uint8_t s_word_sent_code_wpm[CW_WORD_ATTEMPT_WORDS];
static uint8_t s_word_sent_effective_wpm[CW_WORD_ATTEMPT_WORDS];
static char s_word_copy[CW_WORD_COPY_MAX + 1U];
static char s_word_last_sent[CW_WORD_MAX_LEN_MAX + 1U];
static char s_word_last_answer[CW_WORD_COPY_MAX + 1U];
static uint8_t s_word_current_index;
static uint8_t s_word_current_wpm;
static uint8_t s_word_copy_len;
static bool s_word_last_correct;

static void cw_word_normalize_config(cw_word_config_t *config)
{
    if (config == NULL) {
        return;
    }

    config->start_wpm = cw_trainer_clamp_u8(config->start_wpm, CW_WORD_WPM_MIN, CW_WORD_WPM_MAX);
    config->min_char_wpm =
        cw_trainer_clamp_u8(config->min_char_wpm, CW_WORD_WPM_MIN, CW_WORD_WPM_MAX);
    config->lesson = cw_trainer_clamp_u8(config->lesson, CW_WORD_LESSON_MIN, CW_WORD_LESSON_MAX);
    config->max_word_len =
        cw_trainer_clamp_u8(config->max_word_len, CW_WORD_MAX_LEN_MIN, CW_WORD_MAX_LEN_MAX);
}

static bool cw_word_config_equal(const cw_word_config_t *a, const cw_word_config_t *b)
{
    return a != NULL && b != NULL && a->start_wpm == b->start_wpm &&
           a->min_char_wpm == b->min_char_wpm && a->lesson == b->lesson &&
           a->max_word_len == b->max_word_len;
}

static int cw_word_koch_index(char ch)
{
    char normalized = (char)toupper((unsigned char)ch);

    for (uint8_t i = 0U; CW_TRAINER_KOCH_CHARS[i] != '\0'; ++i) {
        if (CW_TRAINER_KOCH_CHARS[i] == normalized) {
            return i;
        }
    }

    return -1;
}

static uint8_t cw_word_required_lesson(const char *word)
{
    uint8_t required = 0U;

    if (word == NULL || word[0] == '\0') {
        return (uint8_t)(CW_WORD_LESSON_MAX + 1U);
    }

    while (*word != '\0') {
        int idx = cw_word_koch_index(*word);
        if (idx < 0 || audio_service_get_cw_pattern(*word) == NULL) {
            return (uint8_t)(CW_WORD_LESSON_MAX + 1U);
        }

        if ((uint8_t)idx > required) {
            required = (uint8_t)idx;
        }

        ++word;
    }

    return required;
}

static bool cw_word_is_allowed(const char *word)
{
    size_t len;

    if (word == NULL) {
        return false;
    }

    len = strlen(word);
    return len >= CW_WORD_MAX_LEN_MIN && len <= s_word_config.max_word_len &&
           cw_word_required_lesson(word) <= s_word_config.lesson;
}

static void cw_word_clear_copy(void)
{
    s_word_copy[0] = '\0';
    s_word_copy_len = 0U;
}

static void cw_word_clear_last_answer(void)
{
    s_word_last_sent[0] = '\0';
    s_word_last_answer[0] = '\0';
    s_word_last_correct = false;
}

static void cw_word_update_view(void)
{
    cw_word_state_t state = s_word_view.state;

    memset(&s_word_view, 0, sizeof(s_word_view));
    s_word_view.state = state == CW_WORD_STATE_IDLE ? CW_WORD_STATE_READY : state;
    s_word_view.config = s_word_config;
    s_word_view.result = s_word_result;
    s_word_view.current_index = s_word_current_index;
    s_word_view.current_wpm = s_word_current_wpm;
    s_word_view.copy_text = s_word_copy;
    s_word_view.copy_len = s_word_copy_len;
    s_word_view.last_sent_word = s_word_last_sent;
    s_word_view.last_answer = s_word_last_answer;
    s_word_view.last_correct = s_word_last_correct;
}

static void cw_word_set_state(cw_word_state_t state)
{
    s_word_view.state = state;
    cw_word_update_view();
}

static void cw_word_reset_run_stats(void)
{
    s_word_result.score = 0U;
    s_word_result.max_wpm = 0U;
    s_word_result.correct_count = 0U;
    s_word_result.total_words = CW_WORD_ATTEMPT_WORDS;
}

static void cw_word_reset_session(bool stop_playback)
{
    if (stop_playback) {
        audio_service_stop_all();
    }

    memset(s_word_attempt, 0, sizeof(s_word_attempt));
    memset(s_word_sent_code_wpm, 0, sizeof(s_word_sent_code_wpm));
    memset(s_word_sent_effective_wpm, 0, sizeof(s_word_sent_effective_wpm));
    cw_word_clear_copy();
    cw_word_clear_last_answer();
    s_word_current_index = 0U;
    s_word_current_wpm = s_word_config.start_wpm;
    cw_word_set_state(CW_WORD_STATE_READY);
}

static void cw_word_compact_text(const char *in, char *out, size_t out_size)
{
    size_t len = 0U;

    if (out == NULL || out_size == 0U) {
        return;
    }

    out[0] = '\0';

    if (in == NULL) {
        return;
    }

    while (*in != '\0' && len + 1U < out_size) {
        char ch = (char)tolower((unsigned char)*in);
        if (!isspace((unsigned char)ch)) {
            out[len++] = ch;
        }
        ++in;
    }

    out[len] = '\0';
}

static void cw_word_generate_attempt(void)
{
    const char *candidates[sizeof(CW_WORD_BANK) / sizeof(CW_WORD_BANK[0])];
    size_t candidate_count = 0U;

    for (size_t i = 0U; i < sizeof(CW_WORD_BANK) / sizeof(CW_WORD_BANK[0]); ++i) {
        if (cw_word_is_allowed(CW_WORD_BANK[i])) {
            candidates[candidate_count++] = CW_WORD_BANK[i];
        }
    }

    if (candidate_count == 0U) {
        candidates[candidate_count++] = "me";
    }

    for (uint8_t i = 0U; i < CW_WORD_ATTEMPT_WORDS; ++i) {
        size_t idx = cw_trainer_rand_u32() % candidate_count;
        s_word_attempt[i] = candidates[idx];
        s_word_sent_code_wpm[i] = 0U;
        s_word_sent_effective_wpm[i] = 0U;
    }

    ESP_LOGI(TAG,
             "word attempt generated: words=%u candidates=%u lesson=%u max_len=%u",
             (unsigned)CW_WORD_ATTEMPT_WORDS,
             (unsigned)candidate_count,
             (unsigned)s_word_config.lesson,
             (unsigned)s_word_config.max_word_len);
}

static uint8_t cw_word_current_code_wpm(void)
{
    return s_word_current_wpm > s_word_config.min_char_wpm ? s_word_current_wpm
                                                          : s_word_config.min_char_wpm;
}

static void cw_word_play_index(uint8_t index, bool record_speed)
{
    uint8_t code_wpm;
    uint8_t effective_wpm;

    if (index >= CW_WORD_ATTEMPT_WORDS || s_word_attempt[index] == NULL) {
        return;
    }

    if (record_speed) {
        code_wpm = cw_word_current_code_wpm();
        effective_wpm = s_word_current_wpm;
        s_word_sent_code_wpm[index] = code_wpm;
        s_word_sent_effective_wpm[index] = effective_wpm;
    } else {
        code_wpm = s_word_sent_code_wpm[index];
        effective_wpm = s_word_sent_effective_wpm[index];
        if (code_wpm == 0U || effective_wpm == 0U) {
            code_wpm = cw_word_current_code_wpm();
            effective_wpm = s_word_current_wpm;
        }
    }

    audio_service_set_cw_wpm(code_wpm);
    audio_service_set_cw_farnsworth_wpm(effective_wpm);
    audio_service_play_cw_text(s_word_attempt[index]);

    ESP_LOGI(TAG,
             "word play: index=%u word=%s code=%u eff=%u replay=%u",
             (unsigned)index,
             s_word_attempt[index],
             (unsigned)code_wpm,
             (unsigned)effective_wpm,
             record_speed ? 0U : 1U);
}

static void cw_word_finish_attempt(void)
{
    ++s_word_result.attempts;

    if (s_word_result.score > s_word_result.best_score) {
        s_word_result.best_score = s_word_result.score;
    }

    if (s_word_result.max_wpm > s_word_result.best_max_wpm) {
        s_word_result.best_max_wpm = s_word_result.max_wpm;
    }

    ESP_LOGI(TAG,
             "word result: score=%lu max=%u correct=%u/%u attempts=%lu best=%lu/%u",
             (unsigned long)s_word_result.score,
             (unsigned)s_word_result.max_wpm,
             (unsigned)s_word_result.correct_count,
             (unsigned)s_word_result.total_words,
             (unsigned long)s_word_result.attempts,
             (unsigned long)s_word_result.best_score,
             (unsigned)s_word_result.best_max_wpm);

    cw_word_set_state(CW_WORD_STATE_RESULT);
}

void cw_word_mode_init(void)
{
    cw_word_reset_session(false);
}

const cw_word_config_t *cw_word_mode_get_config(void)
{
    return &s_word_config;
}

void cw_word_mode_set_config(const cw_word_config_t *config)
{
    cw_word_config_t next_config;
    bool changed;
    bool was_copying;

    if (config == NULL) {
        return;
    }

    next_config = *config;
    cw_word_normalize_config(&next_config);
    changed = !cw_word_config_equal(&next_config, &s_word_config);
    was_copying = s_word_view.state == CW_WORD_STATE_COPYING;
    s_word_config = next_config;

    if (changed) {
        cw_word_reset_session(was_copying);
    } else {
        cw_word_update_view();
    }

    ESP_LOGI(TAG,
             "word config: start=%u min_char=%u lesson=%u max_len=%u",
             (unsigned)s_word_config.start_wpm,
             (unsigned)s_word_config.min_char_wpm,
             (unsigned)s_word_config.lesson,
             (unsigned)s_word_config.max_word_len);
}

void cw_word_mode_abort(void)
{
    cw_word_reset_session(true);
    ESP_LOGI(TAG, "word attempt aborted");
}

void cw_word_mode_start(void)
{
    cw_word_reset_run_stats();
    cw_word_clear_copy();
    cw_word_clear_last_answer();
    s_word_current_index = 0U;
    s_word_current_wpm = s_word_config.start_wpm;
    cw_word_generate_attempt();
    cw_word_set_state(CW_WORD_STATE_COPYING);
    cw_word_play_index(s_word_current_index, true);
}

bool cw_word_mode_append_char(char ch)
{
    char normalized;

    if (s_word_view.state != CW_WORD_STATE_COPYING) {
        return false;
    }

    if (ch < 32 || ch > 126) {
        return false;
    }

    if (s_word_copy_len >= CW_WORD_COPY_MAX) {
        return false;
    }

    normalized = (char)tolower((unsigned char)ch);
    s_word_copy[s_word_copy_len++] = normalized;
    s_word_copy[s_word_copy_len] = '\0';
    cw_word_update_view();
    return true;
}

void cw_word_mode_backspace(void)
{
    if (s_word_view.state != CW_WORD_STATE_COPYING || s_word_copy_len == 0U) {
        return;
    }

    --s_word_copy_len;
    s_word_copy[s_word_copy_len] = '\0';
    cw_word_update_view();
}

const cw_word_result_t *cw_word_mode_submit(void)
{
    char expected[CW_WORD_MAX_LEN_MAX + 1U];
    char answer[CW_WORD_COPY_MAX + 1U];
    const char *word;
    bool correct;
    uint8_t sent_wpm;

    if (s_word_view.state != CW_WORD_STATE_COPYING ||
        s_word_current_index >= CW_WORD_ATTEMPT_WORDS ||
        s_word_attempt[s_word_current_index] == NULL) {
        return &s_word_result;
    }

    word = s_word_attempt[s_word_current_index];
    cw_word_compact_text(word, expected, sizeof(expected));
    cw_word_compact_text(s_word_copy, answer, sizeof(answer));
    correct = strcmp(expected, answer) == 0;
    sent_wpm = s_word_sent_effective_wpm[s_word_current_index];
    if (sent_wpm == 0U) {
        sent_wpm = s_word_current_wpm;
    }

    snprintf(s_word_last_sent, sizeof(s_word_last_sent), "%s", word);
    snprintf(s_word_last_answer, sizeof(s_word_last_answer), "%s", answer);
    s_word_last_correct = correct;

    if (correct) {
        if (sent_wpm > s_word_result.max_wpm) {
            s_word_result.max_wpm = sent_wpm;
        }
        if (s_word_current_wpm < CW_WORD_WPM_MAX) {
            ++s_word_current_wpm;
        }
        s_word_result.score += (uint32_t)s_word_current_wpm * (uint32_t)strlen(expected);
        ++s_word_result.correct_count;
    } else if (s_word_current_wpm > CW_WORD_WPM_MIN) {
        --s_word_current_wpm;
    }

    ESP_LOGI(TAG,
             "word submit: index=%u word=%s answer=%s correct=%u score=%lu wpm=%u",
             (unsigned)s_word_current_index,
             word,
             answer,
             correct ? 1U : 0U,
             (unsigned long)s_word_result.score,
             (unsigned)s_word_current_wpm);

    ++s_word_current_index;
    cw_word_clear_copy();

    if (s_word_current_index >= CW_WORD_ATTEMPT_WORDS) {
        cw_word_finish_attempt();
        return &s_word_result;
    }

    cw_word_update_view();
    cw_word_play_index(s_word_current_index, true);
    return &s_word_result;
}

void cw_word_mode_replay(void)
{
    if (s_word_view.state != CW_WORD_STATE_COPYING) {
        return;
    }

    cw_word_play_index(s_word_current_index, false);
}

const cw_word_view_t *cw_word_mode_get_view(void)
{
    cw_word_update_view();
    return &s_word_view;
}

void cw_word_mode_load_persisted(const cw_word_config_t *config,
                                    const cw_word_result_t *result)
{
    if (config != NULL) {
        s_word_config = *config;
        cw_word_normalize_config(&s_word_config);
    }

    if (result != NULL) {
        s_word_result = *result;
        s_word_result.total_words = CW_WORD_ATTEMPT_WORDS;
    }

    cw_word_reset_session(false);
}

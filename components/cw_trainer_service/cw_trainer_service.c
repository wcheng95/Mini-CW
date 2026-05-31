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
#define CW_WORD_ATTEMPT_WORDS 25U
#define CW_WORD_WPM_MIN 5U
#define CW_WORD_WPM_MAX 40U
#define CW_WORD_LESSON_MIN 9U
#define CW_WORD_LESSON_MAX 40U
#define CW_WORD_MAX_LEN_MIN 2U
#define CW_WORD_MAX_LEN_MAX 15U
#define CW_WORD_COPY_MAX 32U
#define CW_CALLSIGN_ATTEMPT_CALLS 25U
#define CW_CALLSIGN_WPM_MIN 5U
#define CW_CALLSIGN_WPM_MAX 40U
#define CW_CALLSIGN_MAX_LEN 15U
#define CW_CALLSIGN_COPY_MAX 32U

static const char KOCH_CHARS[] =
    "KMURESNAPTLWI.JZ=FOY,VG5/Q92H38B?47C1D60X";

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

// TODO: Prototype only. Replace this static callsign bank with a procedural
// generator or FATFS-loaded callsigns.txt before public release.
static const char *const CW_CALLSIGN_BANK[] = {
    "1A0C",       "1A0KM",      "1A4A",       "1B1AB",
    "2A0APF/P",   "2A0CCC/P",   "2A/DJ6AU",   "2C0VSW",
    "2C3YOL",     "2C4BVJ",     "2E0ADR",     "2E0ANS",
    "2E0AOK",     "2E0AOT",     "2E0AOZ",     "2E0APH",
    "2E0AYQ",     "2E0BBP",     "2E0CNJ",     "2E0CVN",
    "2E0CVN/2ZE", "2E0CVN/NHS", "2E0CVN/P",   "2E0FCK",
    "2E0FNU",     "2E0FTD/P",   "2E0FTG",     "2E0IBM",
    "2E0IFJ",     "2E0IHM",     "2E0KOP",     "2E0NPB",
    "2E0OBO",     "2E0OBO/P",   "2E0ODO",     "2E0OOO",
    "2E0PIE",     "2E0PLE",     "2E0PLW",     "2E0RAF",
    "2E0RNK",     "2E0ROB",     "2E0TJU",     "2E0WAR",
    "2E0YAO",     "2E0ZAI",     "2E1FVS",     "2E1OKT",
    "2E1RAF",     "2I0EUV",     "2I0SAI",     "2M0GUI",
    "2M0SNT",     "2Q0CVN/70",  "2S4CXM",     "2S4SID",
    "2U0ARE",     "2U0ARE/2K",  "2W0ASJ",     "2W0DAA",
    "2W1RSS",     "3A2LS",      "3A2MD",      "3A2MW",
    "3A2NC",      "3A/DL2JRM/P","3A/DL3OCH",  "3A/DL5SE",
    "3A/EA3NT",   "3A/F5NHJ",   "3A/G4PVM",   "3A/HB9APJ",
    "3A/IK1YLL",  "3A/IK2YSE",  "3A/K3OX",    "3A/LX9EG/P",
    "3A/N0FW",    "3A/RA9USU",  "3A/SP3FYM",  "3A/SP9PT",
    "3A/W0YR",    "3B7C",       "3B7M",       "3B7SP",
    "3B8CF",      "3B8/DL7DF",  "3B8FG",      "3B8FQ",
    "3B8/G3TXF",  "3B8M",       "3B8MM",      "3B8XF",
    "3B9C",       "3B9FR",      "3C0W",       "3C3W",
    "3D2R",       "3D2XA",      "3DA0RU",     "3DA0VB",
    "3DA0ZO",     "3E1A",       "3G1X",       "3V1A",
    "3V2A",       "3V6T",       "3V7A",       "3V8BB",
    "3V8CB",      "3V8DLH",     "3V8SF",      "3V8SF/P",
    "3V8SN",      "3V8SQ",      "3V8SS",      "3W22S",
    "3W3W",       "3W7CW",      "3W9JR",      "3X5A",
    "3XD2Z",      "3Y0X",       "3Z0BLY",     "3Z0CWZ",
    "3Z0GI",      "3Z0HNY",     "3Z0ILQ",     "3Z0LH",
};

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

static cw_callsign_config_t s_callsign_config = {
    .start_wpm = 20,
    .min_char_wpm = 10,
    .max_wpm = 40,
};

static cw_callsign_result_t s_callsign_result = {
    .score = 0,
    .max_wpm = 0,
    .correct_count = 0,
    .total_calls = CW_CALLSIGN_ATTEMPT_CALLS,
    .attempts = 0,
    .best_score = 0,
    .best_max_wpm = 0,
};

static cw_callsign_view_t s_callsign_view;
static const char *s_callsign_attempt[CW_CALLSIGN_ATTEMPT_CALLS];
static uint8_t s_callsign_sent_code_wpm[CW_CALLSIGN_ATTEMPT_CALLS];
static uint8_t s_callsign_sent_effective_wpm[CW_CALLSIGN_ATTEMPT_CALLS];
static char s_callsign_copy[CW_CALLSIGN_COPY_MAX + 1U];
static char s_callsign_last_sent[CW_CALLSIGN_MAX_LEN + 1U];
static char s_callsign_last_answer[CW_CALLSIGN_COPY_MAX + 1U];
static uint8_t s_callsign_current_index;
static uint8_t s_callsign_current_wpm;
static uint8_t s_callsign_copy_len;
static bool s_callsign_last_correct;

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

static void cw_word_normalize_config(cw_word_config_t *config)
{
    if (config == NULL) {
        return;
    }

    config->start_wpm = clamp_u8(config->start_wpm, CW_WORD_WPM_MIN, CW_WORD_WPM_MAX);
    config->min_char_wpm =
        clamp_u8(config->min_char_wpm, CW_WORD_WPM_MIN, CW_WORD_WPM_MAX);
    config->lesson = clamp_u8(config->lesson, CW_WORD_LESSON_MIN, CW_WORD_LESSON_MAX);
    config->max_word_len =
        clamp_u8(config->max_word_len, CW_WORD_MAX_LEN_MIN, CW_WORD_MAX_LEN_MAX);
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

    for (uint8_t i = 0U; KOCH_CHARS[i] != '\0'; ++i) {
        if (KOCH_CHARS[i] == normalized) {
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
        size_t idx = cw_lesson_rand_u32() % candidate_count;
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

static void cw_callsign_normalize_config(cw_callsign_config_t *config)
{
    if (config == NULL) {
        return;
    }

    config->max_wpm = clamp_u8(config->max_wpm, CW_CALLSIGN_WPM_MIN, CW_CALLSIGN_WPM_MAX);
    config->start_wpm = clamp_u8(config->start_wpm, CW_CALLSIGN_WPM_MIN, CW_CALLSIGN_WPM_MAX);
    config->min_char_wpm =
        clamp_u8(config->min_char_wpm, CW_CALLSIGN_WPM_MIN, CW_CALLSIGN_WPM_MAX);

    if (config->start_wpm > config->max_wpm) {
        config->start_wpm = config->max_wpm;
    }
}

static bool cw_callsign_config_equal(const cw_callsign_config_t *a,
                                     const cw_callsign_config_t *b)
{
    return a != NULL && b != NULL && a->start_wpm == b->start_wpm &&
           a->min_char_wpm == b->min_char_wpm && a->max_wpm == b->max_wpm;
}

static bool cw_callsign_char_supported(char ch)
{
    char normalized = (char)toupper((unsigned char)ch);

    return ((normalized >= 'A' && normalized <= 'Z') ||
            (normalized >= '0' && normalized <= '9') || normalized == '/') &&
           audio_service_get_cw_pattern(normalized) != NULL;
}

static bool cw_callsign_is_supported(const char *call)
{
    size_t len;

    if (call == NULL) {
        return false;
    }

    len = strlen(call);
    if (len < 3U || len > CW_CALLSIGN_MAX_LEN) {
        return false;
    }

    while (*call != '\0') {
        if (!cw_callsign_char_supported(*call)) {
            return false;
        }
        ++call;
    }

    return true;
}

static void cw_callsign_clear_copy(void)
{
    s_callsign_copy[0] = '\0';
    s_callsign_copy_len = 0U;
}

static void cw_callsign_clear_last_answer(void)
{
    s_callsign_last_sent[0] = '\0';
    s_callsign_last_answer[0] = '\0';
    s_callsign_last_correct = false;
}

static void cw_callsign_update_view(void)
{
    cw_callsign_state_t state = s_callsign_view.state;

    memset(&s_callsign_view, 0, sizeof(s_callsign_view));
    s_callsign_view.state = state == CW_CALLSIGN_STATE_IDLE ? CW_CALLSIGN_STATE_READY : state;
    s_callsign_view.config = s_callsign_config;
    s_callsign_view.result = s_callsign_result;
    s_callsign_view.current_index = s_callsign_current_index;
    s_callsign_view.current_wpm = s_callsign_current_wpm;
    s_callsign_view.copy_text = s_callsign_copy;
    s_callsign_view.copy_len = s_callsign_copy_len;
    s_callsign_view.last_sent_call = s_callsign_last_sent;
    s_callsign_view.last_answer = s_callsign_last_answer;
    s_callsign_view.last_correct = s_callsign_last_correct;
}

static void cw_callsign_set_state(cw_callsign_state_t state)
{
    s_callsign_view.state = state;
    cw_callsign_update_view();
}

static void cw_callsign_reset_run_stats(void)
{
    s_callsign_result.score = 0U;
    s_callsign_result.max_wpm = 0U;
    s_callsign_result.correct_count = 0U;
    s_callsign_result.total_calls = CW_CALLSIGN_ATTEMPT_CALLS;
}

static void cw_callsign_reset_session(bool stop_playback)
{
    if (stop_playback) {
        audio_service_stop_all();
    }

    memset(s_callsign_attempt, 0, sizeof(s_callsign_attempt));
    memset(s_callsign_sent_code_wpm, 0, sizeof(s_callsign_sent_code_wpm));
    memset(s_callsign_sent_effective_wpm, 0, sizeof(s_callsign_sent_effective_wpm));
    cw_callsign_clear_copy();
    cw_callsign_clear_last_answer();
    s_callsign_current_index = 0U;
    s_callsign_current_wpm = s_callsign_config.start_wpm;
    cw_callsign_set_state(CW_CALLSIGN_STATE_READY);
}

static void cw_callsign_compact_text(const char *in, char *out, size_t out_size)
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
        char ch = (char)toupper((unsigned char)*in);
        if (!isspace((unsigned char)ch)) {
            out[len++] = ch;
        }
        ++in;
    }

    out[len] = '\0';
}

static void cw_callsign_generate_attempt(void)
{
    const char *candidates[sizeof(CW_CALLSIGN_BANK) / sizeof(CW_CALLSIGN_BANK[0])];
    size_t candidate_count = 0U;

    for (size_t i = 0U; i < sizeof(CW_CALLSIGN_BANK) / sizeof(CW_CALLSIGN_BANK[0]); ++i) {
        if (cw_callsign_is_supported(CW_CALLSIGN_BANK[i])) {
            candidates[candidate_count++] = CW_CALLSIGN_BANK[i];
        }
    }

    if (candidate_count == 0U) {
        candidates[candidate_count++] = "K1ABC";
    }

    for (uint8_t i = 0U; i < CW_CALLSIGN_ATTEMPT_CALLS; ++i) {
        size_t idx = cw_lesson_rand_u32() % candidate_count;
        s_callsign_attempt[i] = candidates[idx];
        s_callsign_sent_code_wpm[i] = 0U;
        s_callsign_sent_effective_wpm[i] = 0U;
    }

    ESP_LOGI(TAG,
             "callsign attempt generated: calls=%u candidates=%u",
             (unsigned)CW_CALLSIGN_ATTEMPT_CALLS,
             (unsigned)candidate_count);
}

static uint8_t cw_callsign_current_code_wpm(void)
{
    return s_callsign_current_wpm > s_callsign_config.min_char_wpm
               ? s_callsign_current_wpm
               : s_callsign_config.min_char_wpm;
}

static void cw_callsign_play_index(uint8_t index, bool record_speed)
{
    uint8_t code_wpm;
    uint8_t effective_wpm;

    if (index >= CW_CALLSIGN_ATTEMPT_CALLS || s_callsign_attempt[index] == NULL) {
        return;
    }

    if (record_speed) {
        code_wpm = cw_callsign_current_code_wpm();
        effective_wpm = s_callsign_current_wpm;
        s_callsign_sent_code_wpm[index] = code_wpm;
        s_callsign_sent_effective_wpm[index] = effective_wpm;
    } else {
        code_wpm = s_callsign_sent_code_wpm[index];
        effective_wpm = s_callsign_sent_effective_wpm[index];
        if (code_wpm == 0U || effective_wpm == 0U) {
            code_wpm = cw_callsign_current_code_wpm();
            effective_wpm = s_callsign_current_wpm;
        }
    }

    audio_service_set_cw_wpm(code_wpm);
    audio_service_set_cw_farnsworth_wpm(effective_wpm);
    audio_service_play_cw_text(s_callsign_attempt[index]);

    ESP_LOGI(TAG,
             "callsign play: index=%u call=%s code=%u eff=%u replay=%u",
             (unsigned)index,
             s_callsign_attempt[index],
             (unsigned)code_wpm,
             (unsigned)effective_wpm,
             record_speed ? 0U : 1U);
}

static void cw_callsign_finish_attempt(void)
{
    ++s_callsign_result.attempts;

    if (s_callsign_result.score > s_callsign_result.best_score) {
        s_callsign_result.best_score = s_callsign_result.score;
    }

    if (s_callsign_result.max_wpm > s_callsign_result.best_max_wpm) {
        s_callsign_result.best_max_wpm = s_callsign_result.max_wpm;
    }

    ESP_LOGI(TAG,
             "callsign result: score=%lu max=%u correct=%u/%u attempts=%lu best=%lu/%u",
             (unsigned long)s_callsign_result.score,
             (unsigned)s_callsign_result.max_wpm,
             (unsigned)s_callsign_result.correct_count,
             (unsigned)s_callsign_result.total_calls,
             (unsigned long)s_callsign_result.attempts,
             (unsigned long)s_callsign_result.best_score,
             (unsigned)s_callsign_result.best_max_wpm);

    cw_callsign_set_state(CW_CALLSIGN_STATE_RESULT);
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
    cw_word_reset_session(false);
    cw_callsign_reset_session(false);

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

const cw_word_config_t *cw_trainer_word_get_config(void)
{
    return &s_word_config;
}

void cw_trainer_word_set_config(const cw_word_config_t *config)
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

void cw_trainer_word_abort(void)
{
    cw_word_reset_session(true);
    ESP_LOGI(TAG, "word attempt aborted");
}

void cw_trainer_word_start(void)
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

bool cw_trainer_word_append_char(char ch)
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

void cw_trainer_word_backspace(void)
{
    if (s_word_view.state != CW_WORD_STATE_COPYING || s_word_copy_len == 0U) {
        return;
    }

    --s_word_copy_len;
    s_word_copy[s_word_copy_len] = '\0';
    cw_word_update_view();
}

const cw_word_result_t *cw_trainer_word_submit(void)
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

void cw_trainer_word_replay(void)
{
    if (s_word_view.state != CW_WORD_STATE_COPYING) {
        return;
    }

    cw_word_play_index(s_word_current_index, false);
}

const cw_word_view_t *cw_trainer_word_get_view(void)
{
    cw_word_update_view();
    return &s_word_view;
}

void cw_trainer_word_load_persisted(const cw_word_config_t *config,
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

const cw_callsign_config_t *cw_trainer_callsign_get_config(void)
{
    return &s_callsign_config;
}

void cw_trainer_callsign_set_config(const cw_callsign_config_t *config)
{
    cw_callsign_config_t next_config;
    bool changed;
    bool was_copying;

    if (config == NULL) {
        return;
    }

    next_config = *config;
    cw_callsign_normalize_config(&next_config);
    changed = !cw_callsign_config_equal(&next_config, &s_callsign_config);
    was_copying = s_callsign_view.state == CW_CALLSIGN_STATE_COPYING;
    s_callsign_config = next_config;

    if (changed) {
        cw_callsign_reset_session(was_copying);
    } else {
        cw_callsign_update_view();
    }

    ESP_LOGI(TAG,
             "callsign config: start=%u min_char=%u max=%u",
             (unsigned)s_callsign_config.start_wpm,
             (unsigned)s_callsign_config.min_char_wpm,
             (unsigned)s_callsign_config.max_wpm);
}

void cw_trainer_callsign_abort(void)
{
    cw_callsign_reset_session(true);
    ESP_LOGI(TAG, "callsign attempt aborted");
}

void cw_trainer_callsign_start(void)
{
    cw_callsign_reset_run_stats();
    cw_callsign_clear_copy();
    cw_callsign_clear_last_answer();
    s_callsign_current_index = 0U;
    s_callsign_current_wpm = s_callsign_config.start_wpm;
    cw_callsign_generate_attempt();
    cw_callsign_set_state(CW_CALLSIGN_STATE_COPYING);
    cw_callsign_play_index(s_callsign_current_index, true);
}

bool cw_trainer_callsign_append_char(char ch)
{
    char normalized;

    if (s_callsign_view.state != CW_CALLSIGN_STATE_COPYING) {
        return false;
    }

    normalized = (char)toupper((unsigned char)ch);
    if (!cw_callsign_char_supported(normalized)) {
        return false;
    }

    if (s_callsign_copy_len >= CW_CALLSIGN_COPY_MAX) {
        return false;
    }

    s_callsign_copy[s_callsign_copy_len++] = normalized;
    s_callsign_copy[s_callsign_copy_len] = '\0';
    cw_callsign_update_view();
    return true;
}

void cw_trainer_callsign_backspace(void)
{
    if (s_callsign_view.state != CW_CALLSIGN_STATE_COPYING || s_callsign_copy_len == 0U) {
        return;
    }

    --s_callsign_copy_len;
    s_callsign_copy[s_callsign_copy_len] = '\0';
    cw_callsign_update_view();
}

const cw_callsign_result_t *cw_trainer_callsign_submit(void)
{
    char expected[CW_CALLSIGN_MAX_LEN + 1U];
    char answer[CW_CALLSIGN_COPY_MAX + 1U];
    const char *call;
    bool correct;
    uint8_t sent_wpm;

    if (s_callsign_view.state != CW_CALLSIGN_STATE_COPYING ||
        s_callsign_current_index >= CW_CALLSIGN_ATTEMPT_CALLS ||
        s_callsign_attempt[s_callsign_current_index] == NULL) {
        return &s_callsign_result;
    }

    call = s_callsign_attempt[s_callsign_current_index];
    cw_callsign_compact_text(call, expected, sizeof(expected));
    cw_callsign_compact_text(s_callsign_copy, answer, sizeof(answer));
    correct = strcmp(expected, answer) == 0;
    sent_wpm = s_callsign_sent_effective_wpm[s_callsign_current_index];
    if (sent_wpm == 0U) {
        sent_wpm = s_callsign_current_wpm;
    }

    snprintf(s_callsign_last_sent, sizeof(s_callsign_last_sent), "%s", call);
    snprintf(s_callsign_last_answer, sizeof(s_callsign_last_answer), "%s", answer);
    s_callsign_last_correct = correct;

    if (correct) {
        if (sent_wpm > s_callsign_result.max_wpm) {
            s_callsign_result.max_wpm = sent_wpm;
        }
        if (s_callsign_current_wpm < s_callsign_config.max_wpm) {
            ++s_callsign_current_wpm;
        }
        s_callsign_result.score +=
            (uint32_t)sent_wpm * (uint32_t)strlen(expected);
        ++s_callsign_result.correct_count;
    } else if (s_callsign_current_wpm > CW_CALLSIGN_WPM_MIN) {
        --s_callsign_current_wpm;
    }

    ESP_LOGI(TAG,
             "callsign submit: index=%u call=%s answer=%s correct=%u score=%lu wpm=%u",
             (unsigned)s_callsign_current_index,
             call,
             answer,
             correct ? 1U : 0U,
             (unsigned long)s_callsign_result.score,
             (unsigned)s_callsign_current_wpm);

    ++s_callsign_current_index;
    cw_callsign_clear_copy();

    if (s_callsign_current_index >= CW_CALLSIGN_ATTEMPT_CALLS) {
        cw_callsign_finish_attempt();
        return &s_callsign_result;
    }

    cw_callsign_update_view();
    cw_callsign_play_index(s_callsign_current_index, true);
    return &s_callsign_result;
}

void cw_trainer_callsign_replay(void)
{
    if (s_callsign_view.state != CW_CALLSIGN_STATE_COPYING) {
        return;
    }

    cw_callsign_play_index(s_callsign_current_index, false);
}

const cw_callsign_view_t *cw_trainer_callsign_get_view(void)
{
    cw_callsign_update_view();
    return &s_callsign_view;
}

void cw_trainer_callsign_load_persisted(const cw_callsign_config_t *config,
                                        const cw_callsign_result_t *result)
{
    if (config != NULL) {
        s_callsign_config = *config;
        cw_callsign_normalize_config(&s_callsign_config);
    }

    if (result != NULL) {
        s_callsign_result = *result;
        s_callsign_result.total_calls = CW_CALLSIGN_ATTEMPT_CALLS;
    }

    cw_callsign_reset_session(false);
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

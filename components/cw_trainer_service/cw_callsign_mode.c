/*
 * Mode implementation for cw_trainer_service.
 * Private to the cw_trainer_service component.
 */

#include "cw_callsign_mode.h"
#include "cw_trainer_internal.h"

#include "audio_service.h"
#include "esp_log.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "cw_callsign_mode";
#define CW_CALLSIGN_ATTEMPT_CALLS 25U
#define CW_CALLSIGN_WPM_MIN 5U
#define CW_CALLSIGN_WPM_MAX 40U
#define CW_CALLSIGN_MAX_LEN 15U
#define CW_CALLSIGN_COPY_MAX 32U

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

static void cw_callsign_normalize_config(cw_callsign_config_t *config)
{
    if (config == NULL) {
        return;
    }

    config->max_wpm = cw_trainer_clamp_u8(config->max_wpm, CW_CALLSIGN_WPM_MIN, CW_CALLSIGN_WPM_MAX);
    config->start_wpm = cw_trainer_clamp_u8(config->start_wpm, CW_CALLSIGN_WPM_MIN, CW_CALLSIGN_WPM_MAX);
    config->min_char_wpm =
        cw_trainer_clamp_u8(config->min_char_wpm, CW_CALLSIGN_WPM_MIN, CW_CALLSIGN_WPM_MAX);

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
        size_t idx = cw_trainer_rand_u32() % candidate_count;
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

void cw_callsign_mode_init(void)
{
    cw_callsign_reset_session(false);
}

const cw_callsign_config_t *cw_callsign_mode_get_config(void)
{
    return &s_callsign_config;
}

void cw_callsign_mode_set_config(const cw_callsign_config_t *config)
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

void cw_callsign_mode_abort(void)
{
    cw_callsign_reset_session(true);
    ESP_LOGI(TAG, "callsign attempt aborted");
}

void cw_callsign_mode_start(void)
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

bool cw_callsign_mode_append_char(char ch)
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

void cw_callsign_mode_backspace(void)
{
    if (s_callsign_view.state != CW_CALLSIGN_STATE_COPYING || s_callsign_copy_len == 0U) {
        return;
    }

    --s_callsign_copy_len;
    s_callsign_copy[s_callsign_copy_len] = '\0';
    cw_callsign_update_view();
}

const cw_callsign_result_t *cw_callsign_mode_submit(void)
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

void cw_callsign_mode_replay(void)
{
    if (s_callsign_view.state != CW_CALLSIGN_STATE_COPYING) {
        return;
    }

    cw_callsign_play_index(s_callsign_current_index, false);
}

const cw_callsign_view_t *cw_callsign_mode_get_view(void)
{
    cw_callsign_update_view();
    return &s_callsign_view;
}

void cw_callsign_mode_load_persisted(const cw_callsign_config_t *config,
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

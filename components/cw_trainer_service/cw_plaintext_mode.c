/*
 * Mode implementation for cw_trainer_service.
 * Private to the cw_trainer_service component.
 */

#include "cw_plaintext_mode.h"
#include "cw_trainer_internal.h"

#include "audio_service.h"
#include "esp_log.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "cw_plaintext_mode";
#define CW_PLAINTEXT_WPM_MIN 5U
#define CW_PLAINTEXT_WPM_MAX 40U
#define CW_PLAINTEXT_TARGET_MAX 512U
#define CW_PLAINTEXT_COPY_MAX 512U

typedef struct {
    const char *title;
    const char *source;
    const char *text;
} cw_plaintext_bank_entry_t;

// TODO: Prototype only. Replace this static plaintext bank with FATFS-loaded
// plaintext.txt or larger SQL-derived collections before public release.
static const cw_plaintext_bank_entry_t CW_PLAINTEXT_BANK[] = {
    {
        "Fortune 1",
        "LCWO style",
        "A DAY FOR FIRM DECISIONS. OR IS IT?",
    },
    {
        "Fortune 2",
        "LCWO style",
        "A FEW HOURS GRACE BEFORE THE MADNESS BEGINS AGAIN.",
    },
    {
        "Radio room",
        "Mini-CW",
        "A QUIET RADIO ROOM CAN MAKE WEAK SIGNALS SOUND CLEAR.",
    },
    {
        "Copy work",
        "Mini-CW",
        "COPY THE TEXT AS YOU HEAR IT AND KEEP A STEADY RHYTHM.",
    },
    {
        "Band open",
        "Mini-CW",
        "THE WEATHER IS FAIR, THE BAND IS OPEN, AND THE COFFEE IS HOT.",
    },
    {
        "CQ",
        "Mini-CW",
        "CQ CQ DE MINI CW. PLAIN TEXT TRAINING STARTS NOW.",
    },
    {
        "Clean copy",
        "Mini-CW",
        "SPEED IS USEFUL, BUT CLEAN COPY IS BETTER.",
    },
    {
        "Alphabet",
        "Mini-CW",
        "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG.",
    },
    {
        "Report",
        "Mini-CW",
        "REPORT IS 579 IN OREGON. NAME IS PAT.",
    },
    {
        "Message",
        "Mini-CW",
        "KEEP THE KEYER READY AND LISTEN FOR THE NEXT MESSAGE.",
    },
    {
        "Operators",
        "Mini-CW",
        "GOOD OPERATORS SEND CLEAR CODE AT A COMFORTABLE SPEED.",
    },
    {
        "Practice",
        "Mini-CW",
        "THIS IS A SHORT PRACTICE TEXT FOR COPYING WORDS IN ORDER.",
    },
    {
        "Spaces",
        "Mini-CW",
        "AN EXTRA SPACE WILL NOT MATTER AFTER NORMALIZING THE COPY.",
    },
    {
        "Waves",
        "Mini-CW",
        "RADIO WAVES CROSS MOUNTAINS, RIVERS, AND QUIET TOWNS.",
    },
    {
        "Question",
        "Mini-CW",
        "CAN YOU COPY THIS QUESTION WITHOUT LOSING THE LAST WORD?",
    },
    {
        "Final mode",
        "Mini-CW",
        "THE FINAL MODE BRINGS PLAIN LANGUAGE PRACTICE TO MINI CW.",
    },
    {
        "QRS",
        "Mini-CW",
        "SEND QRS IF NEEDED, THEN TRY AGAIN AT A HIGHER SPEED.",
    },
    {
        "Test",
        "Mini-CW",
        "N0CALL DE K1ABC = TEST MESSAGE NUMBER 5.",
    },
    {
        "Bench",
        "Mini-CW",
        "MEASURE TWICE, SOLDER ONCE, AND CHECK THE POWER.",
    },
    {
        "LCWO",
        "Mini-CW",
        "LCWO STYLE PLAIN TEXT USES ONE MESSAGE AND ONE RESULT.",
    },
};

static cw_plaintext_config_t s_plaintext_config = {
    .code_wpm = 20,
    .effective_wpm = 12,
};

static cw_plaintext_result_t s_plaintext_result = {
    .target_chars = 0,
    .copy_chars = 0,
    .errors = 0,
    .accuracy_tenths = 0,
    .attempts = 0,
    .best_accuracy_tenths = 0,
    .last_accuracy_tenths = 0,
};

static cw_plaintext_view_t s_plaintext_view;
static char s_plaintext_target[CW_PLAINTEXT_TARGET_MAX + 1U];
static char s_plaintext_copy[CW_PLAINTEXT_COPY_MAX + 1U];
static const char *s_plaintext_title = "";
static const char *s_plaintext_source = "";
static uint16_t s_plaintext_target_len;
static uint16_t s_plaintext_copy_len;

static void cw_plaintext_normalize_config(cw_plaintext_config_t *config)
{
    if (config == NULL) {
        return;
    }

    config->code_wpm =
        cw_trainer_clamp_u8(config->code_wpm, CW_PLAINTEXT_WPM_MIN, CW_PLAINTEXT_WPM_MAX);
    config->effective_wpm =
        cw_trainer_clamp_u8(config->effective_wpm, CW_PLAINTEXT_WPM_MIN, CW_PLAINTEXT_WPM_MAX);

    if (config->effective_wpm > config->code_wpm) {
        config->effective_wpm = config->code_wpm;
    }
}

static bool cw_plaintext_config_equal(const cw_plaintext_config_t *a,
                                      const cw_plaintext_config_t *b)
{
    return a != NULL && b != NULL && a->code_wpm == b->code_wpm &&
           a->effective_wpm == b->effective_wpm;
}

static bool cw_plaintext_char_supported(char ch)
{
    char normalized = (char)toupper((unsigned char)ch);

    if (normalized == ' ') {
        return true;
    }

    return audio_service_get_cw_pattern(normalized) != NULL;
}

static char cw_plaintext_normalize_char(char ch)
{
    char normalized = (char)toupper((unsigned char)ch);

    if (normalized == ';') {
        normalized = '?';
    }

    if (isspace((unsigned char)normalized)) {
        normalized = ' ';
    }

    return normalized;
}

static void cw_plaintext_normalize_text(const char *in, char *out, size_t out_size)
{
    bool pending_space = false;
    size_t len = 0U;

    if (out == NULL || out_size == 0U) {
        return;
    }

    out[0] = '\0';

    if (in == NULL) {
        return;
    }

    while (*in != '\0' && len + 1U < out_size) {
        char ch = cw_plaintext_normalize_char(*in);

        if (ch == ' ') {
            pending_space = len > 0U;
        } else if (cw_plaintext_char_supported(ch)) {
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

static void cw_plaintext_clear_copy(void)
{
    s_plaintext_copy[0] = '\0';
    s_plaintext_copy_len = 0U;
}

static void cw_plaintext_update_view(void)
{
    cw_plaintext_state_t state = s_plaintext_view.state;

    memset(&s_plaintext_view, 0, sizeof(s_plaintext_view));
    s_plaintext_view.state =
        state == CW_PLAINTEXT_STATE_IDLE ? CW_PLAINTEXT_STATE_READY : state;
    s_plaintext_view.config = s_plaintext_config;
    s_plaintext_view.result = s_plaintext_result;
    s_plaintext_view.title = s_plaintext_title;
    s_plaintext_view.source = s_plaintext_source;
    s_plaintext_view.target_text = s_plaintext_target;
    s_plaintext_view.copy_text = s_plaintext_copy;
    s_plaintext_view.target_len = s_plaintext_target_len;
    s_plaintext_view.copy_len = s_plaintext_copy_len;
}

static void cw_plaintext_set_state(cw_plaintext_state_t state)
{
    s_plaintext_view.state = state;
    cw_plaintext_update_view();
}

static void cw_plaintext_reset_session(bool stop_playback)
{
    if (stop_playback) {
        audio_service_stop_all();
    }

    s_plaintext_target[0] = '\0';
    s_plaintext_target_len = 0U;
    s_plaintext_title = "";
    s_plaintext_source = "";
    cw_plaintext_clear_copy();
    cw_plaintext_set_state(CW_PLAINTEXT_STATE_READY);
}

static void cw_plaintext_generate_target(void)
{
    size_t entry_count = sizeof(CW_PLAINTEXT_BANK) / sizeof(CW_PLAINTEXT_BANK[0]);
    size_t idx = entry_count > 0U ? cw_trainer_rand_u32() % entry_count : 0U;
    const cw_plaintext_bank_entry_t *entry =
        entry_count > 0U ? &CW_PLAINTEXT_BANK[idx] : NULL;

    if (entry == NULL) {
        s_plaintext_title = "Fallback";
        s_plaintext_source = "Mini-CW";
        cw_plaintext_normalize_text("CQ MINI CW", s_plaintext_target, sizeof(s_plaintext_target));
    } else {
        s_plaintext_title = entry->title;
        s_plaintext_source = entry->source;
        cw_plaintext_normalize_text(entry->text, s_plaintext_target, sizeof(s_plaintext_target));
    }

    if (s_plaintext_target[0] == '\0') {
        s_plaintext_title = "Fallback";
        s_plaintext_source = "Mini-CW";
        cw_plaintext_normalize_text("CQ MINI CW", s_plaintext_target, sizeof(s_plaintext_target));
    }

    s_plaintext_target_len = (uint16_t)strlen(s_plaintext_target);
    ESP_LOGI(TAG,
             "plaintext generated: title=%s chars=%u",
             s_plaintext_title,
             (unsigned)s_plaintext_target_len);
}

void cw_plaintext_mode_init(void)
{
    cw_plaintext_reset_session(false);
}

const cw_plaintext_config_t *cw_plaintext_mode_get_config(void)
{
    return &s_plaintext_config;
}

void cw_plaintext_mode_set_config(const cw_plaintext_config_t *config)
{
    cw_plaintext_config_t next_config;
    bool changed;
    bool was_copying;

    if (config == NULL) {
        return;
    }

    next_config = *config;
    cw_plaintext_normalize_config(&next_config);
    changed = !cw_plaintext_config_equal(&next_config, &s_plaintext_config);
    was_copying = s_plaintext_view.state == CW_PLAINTEXT_STATE_COPYING;
    s_plaintext_config = next_config;

    if (changed) {
        cw_plaintext_reset_session(was_copying);
    } else {
        cw_plaintext_update_view();
    }

    ESP_LOGI(TAG,
             "plaintext config: code=%u eff=%u",
             (unsigned)s_plaintext_config.code_wpm,
             (unsigned)s_plaintext_config.effective_wpm);
}

void cw_plaintext_mode_abort(void)
{
    cw_plaintext_reset_session(true);
    ESP_LOGI(TAG, "plaintext attempt aborted");
}

void cw_plaintext_mode_start(void)
{
    cw_plaintext_clear_copy();
    cw_plaintext_generate_target();

    audio_service_set_cw_wpm(s_plaintext_config.code_wpm);
    audio_service_set_cw_farnsworth_wpm(s_plaintext_config.effective_wpm);
    audio_service_play_cw_text(s_plaintext_target);
    cw_plaintext_set_state(CW_PLAINTEXT_STATE_COPYING);
}

bool cw_plaintext_mode_append_char(char ch)
{
    char normalized;

    if (s_plaintext_view.state != CW_PLAINTEXT_STATE_COPYING) {
        return false;
    }

    normalized = cw_plaintext_normalize_char(ch);
    if (!cw_plaintext_char_supported(normalized)) {
        return false;
    }

    if (s_plaintext_copy_len >= CW_PLAINTEXT_COPY_MAX) {
        return false;
    }

    s_plaintext_copy[s_plaintext_copy_len++] = normalized;
    s_plaintext_copy[s_plaintext_copy_len] = '\0';
    cw_plaintext_update_view();
    return true;
}

void cw_plaintext_mode_backspace(void)
{
    if (s_plaintext_view.state != CW_PLAINTEXT_STATE_COPYING || s_plaintext_copy_len == 0U) {
        return;
    }

    --s_plaintext_copy_len;
    s_plaintext_copy[s_plaintext_copy_len] = '\0';
    cw_plaintext_update_view();
}

const cw_plaintext_result_t *cw_plaintext_mode_submit(void)
{
    char target[CW_PLAINTEXT_TARGET_MAX + 1U];
    char copy[CW_PLAINTEXT_COPY_MAX + 1U];
    uint16_t target_chars;
    uint16_t distance;
    uint16_t accuracy_tenths = 0U;

    if (s_plaintext_view.state != CW_PLAINTEXT_STATE_COPYING) {
        return &s_plaintext_result;
    }

    cw_plaintext_normalize_text(s_plaintext_target, target, sizeof(target));
    cw_plaintext_normalize_text(s_plaintext_copy, copy, sizeof(copy));

    target_chars = (uint16_t)strlen(target);
    distance = cw_trainer_levenshtein(target, copy, CW_PLAINTEXT_TARGET_MAX);

    if (target_chars > 0U) {
        uint32_t bounded_distance = distance > target_chars ? target_chars : distance;
        accuracy_tenths =
            (uint16_t)(1000U - ((bounded_distance * 1000U) / target_chars));
    }

    s_plaintext_result.target_chars = target_chars;
    s_plaintext_result.copy_chars = (uint16_t)strlen(copy);
    s_plaintext_result.errors = distance;
    s_plaintext_result.accuracy_tenths = accuracy_tenths;
    s_plaintext_result.last_accuracy_tenths = accuracy_tenths;
    ++s_plaintext_result.attempts;

    if (accuracy_tenths > s_plaintext_result.best_accuracy_tenths) {
        s_plaintext_result.best_accuracy_tenths = accuracy_tenths;
    }

    ESP_LOGI(TAG,
             "plaintext result: accuracy=%u.%u errors=%u target=%u copy=%u attempts=%lu best=%u.%u",
             (unsigned)(accuracy_tenths / 10U),
             (unsigned)(accuracy_tenths % 10U),
             (unsigned)s_plaintext_result.errors,
             (unsigned)s_plaintext_result.target_chars,
             (unsigned)s_plaintext_result.copy_chars,
             (unsigned long)s_plaintext_result.attempts,
             (unsigned)(s_plaintext_result.best_accuracy_tenths / 10U),
             (unsigned)(s_plaintext_result.best_accuracy_tenths % 10U));

    cw_plaintext_set_state(CW_PLAINTEXT_STATE_RESULT);
    return &s_plaintext_result;
}

const cw_plaintext_view_t *cw_plaintext_mode_get_view(void)
{
    cw_plaintext_update_view();
    return &s_plaintext_view;
}

void cw_plaintext_mode_load_persisted(const cw_plaintext_config_t *config,
                                         const cw_plaintext_result_t *result)
{
    if (config != NULL) {
        s_plaintext_config = *config;
        cw_plaintext_normalize_config(&s_plaintext_config);
    }

    if (result != NULL) {
        s_plaintext_result = *result;
    }

    cw_plaintext_reset_session(false);
}

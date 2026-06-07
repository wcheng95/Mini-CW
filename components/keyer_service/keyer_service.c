/*
 * keyer_service
 *
 * Responsibility: Owns paddle/key input abstraction and KeyIn timing.
 * Hardware ownership: paddle/key GPIO. Sidetone is requested through
 * audio_service, not produced here.
 */

#include "keyer_service.h"

#include "keyer_decoder.h"

#include "audio_service.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "keyer_service";

#define KEYER_DEFAULT_TX_WPM 19U
#define KEYER_MIN_TX_WPM 5U
#define KEYER_MAX_TX_WPM 60U
#define KEYER_KEY_IN_MODE_COUNT 4
#define KEYER_KEY_OUT_MODE_COUNT 5
#define KEYER_PADDLE_MODE_COUNT 3
#define KEYER_GPIO_TIP GPIO_NUM_13
#define KEYER_GPIO_RING GPIO_NUM_15
#define KEYER_GPIO_OUT_TIP GPIO_NUM_3
#define KEYER_GPIO_OUT_RING GPIO_NUM_6
#define KEYER_GPIO_ACTIVE_LEVEL 0
#define KEYER_GPIO_INACTIVE_LEVEL 1
#define KEYER_EVENT_RING_CAP 16U
#define KEYER_TX_TEXT_MAX 511U
#define KEYER_TX_DELAY_MIN_S 0U
#define KEYER_TX_DELAY_MAX_S 99U
#define KEYER_TUNE_TIMEOUT_MIN_S 0U
#define KEYER_TUNE_TIMEOUT_MAX_S 20U
#define KEYER_REPEAT_MIN_S 1U
#define KEYER_REPEAT_MAX_S 99U
#define KEYER_SK_WPM_SAVE_STABLE_MS 120000U
#define KEYER_SK_ACTIVE_GAP_MAX_MS 3000U
#define KEYER_SK_STABLE_BAND_WPM 1U
#define KEYER_SK_ADAPT_OLD_WEIGHT 3U
#define KEYER_SK_ADAPT_DENOMINATOR 4U
#define KEYER_OP_CANDIDATE_MAX_LEN 12U
#define KEYER_OP_ROLLING_LEN 16U

typedef enum {
    KEYER_PADDLE_IDLE = 0,
    KEYER_PADDLE_WAIT_GAP,
} keyer_paddle_state_t;

typedef enum {
    KEYER_ELEMENT_NONE = 0,
    KEYER_ELEMENT_DIT,
    KEYER_ELEMENT_DAH,
} keyer_element_t;

static const keyer_event_t KEYER_NO_EVENT = {
    .type = KEYER_EVENT_NONE,
    .decoded_char = '\0',
    .duration_ms = 0,
};

static keyer_event_t s_event_ring[KEYER_EVENT_RING_CAP];
static uint8_t s_event_head;
static uint8_t s_event_tail;
static uint8_t s_event_count;
static keyer_key_in_mode_t s_key_in_mode = KEYER_KEY_IN_PADDLE;
static keyer_key_out_mode_t s_key_out_mode = KEYER_KEY_OUT_PADDLE;
static keyer_paddle_mode_t s_paddle_mode = KEYER_PADDLE_IAMBIC_B;
static uint8_t s_key_in_wpm = KEYER_DEFAULT_TX_WPM;
static uint8_t s_sk_wpm_current = KEYER_DEFAULT_TX_WPM;
static uint16_t s_sk_unit_ms;
static bool s_sk_save_candidate_active;
static uint8_t s_sk_save_candidate_wpm;
static uint32_t s_sk_save_candidate_active_ms;
static TickType_t s_sk_save_candidate_last_tick;
static bool s_sk_wpm_save_requested;
static bool s_gpio_ready;
static bool s_key_out_gpio_ready;
static bool s_straight_tone_on;
static bool s_straight_key_down;
static TickType_t s_straight_key_down_tick;
static bool s_mute;
static bool s_tune_active;
static bool s_tune_latched;
static bool s_tune_output_active;
static bool s_tune_consume_until_release;
static keyer_paddle_state_t s_paddle_state = KEYER_PADDLE_IDLE;
static TickType_t s_paddle_ready_tick;
static TickType_t s_paddle_element_end_tick;
static keyer_element_t s_last_element = KEYER_ELEMENT_NONE;
static bool s_dit_memory;
static bool s_dah_memory;
static bool s_squeeze_latched;
static bool s_mode_b_extra_pending;
static bool s_key_out_element_active;
static TickType_t s_key_out_release_tick;
static bool s_bug_dah_down;
static bool s_cancel_ignore_tip;
static bool s_cancel_ignore_ring;
static keyer_decoder_t s_paddle_decoder;
static TickType_t s_decoder_last_element_end_tick;
static bool s_decoder_char_finalized;
static bool s_decoder_space_emitted;

typedef enum {
    KEYER_TX_IDLE = 0,
    KEYER_TX_GAP,
    KEYER_TX_ELEMENT,
} keyer_tx_state_t;

static char s_tx_text[KEYER_TX_TEXT_MAX + 1U];
static uint16_t s_tx_text_len;
static uint16_t s_tx_text_pos;
static const char *s_tx_pattern;
static uint8_t s_tx_pattern_pos;
static keyer_tx_state_t s_tx_state = KEYER_TX_IDLE;
static TickType_t s_tx_due_tick;
static uint32_t s_tx_revision;
static keyer_op_entry_t *s_op_entries;
static size_t s_op_entry_count;
static char s_op_name[KEYER_OP_NAME_MAX_LEN + 1U];
static char s_op_candidate[KEYER_OP_CANDIDATE_MAX_LEN + 1U];
static uint8_t s_op_candidate_len;
static bool s_op_candidate_has_digit;
static bool s_op_candidate_overflow;
static char s_op_rolling[KEYER_OP_ROLLING_LEN + 1U];

static keyer_config_t s_config = {
    .key_out_mode = KEYER_KEY_OUT_PADDLE,
    .paddle_mode = KEYER_PADDLE_IAMBIC_B,
    .sk_wpm = KEYER_DEFAULT_TX_WPM,
    .tx_delay_s = 1U,
    .tune_timeout_s = 10U,
    .repeat_interval_s = 6U,
    .mycall = "AG6AQ",
    .message = {
        "CQ SOTA DE AG6AQ",
        "TU UR CA CA BK",
        "BK TU 72 DE AG6AQ E E",
        "AG6AQ",
        "BK TU GM UR 599 599 CA CA BK",
    },
};

static void keyer_stop_tx_playback(bool stop_audio);

static uint16_t keyer_clamp_tx_wpm(uint16_t wpm)
{
    if (wpm < KEYER_MIN_TX_WPM) {
        return KEYER_MIN_TX_WPM;
    }

    if (wpm > KEYER_MAX_TX_WPM) {
        return KEYER_MAX_TX_WPM;
    }

    return wpm;
}

static const char *keyer_input_mode_name(keyer_input_mode_t mode)
{
    switch (mode) {
    case KEYER_INPUT_STRAIGHT_KEY:
        return "straight key";
    case KEYER_INPUT_SINGLE_PADDLE:
        return "single paddle";
    case KEYER_INPUT_DUAL_PADDLE:
        return "dual paddle";
    default:
        return "unknown";
    }
}

static keyer_key_in_mode_t keyer_clamp_key_in_mode(keyer_key_in_mode_t mode)
{
    if ((int)mode < 0 || (int)mode >= KEYER_KEY_IN_MODE_COUNT) {
        return KEYER_KEY_IN_PADDLE;
    }

    return mode;
}

static keyer_key_out_mode_t keyer_clamp_key_out_mode(keyer_key_out_mode_t mode)
{
    if ((int)mode < 0 || (int)mode >= KEYER_KEY_OUT_MODE_COUNT) {
        return KEYER_KEY_OUT_PADDLE;
    }

    return mode;
}

static keyer_paddle_mode_t keyer_clamp_paddle_mode(keyer_paddle_mode_t mode)
{
    if ((int)mode < 0 || (int)mode >= KEYER_PADDLE_MODE_COUNT) {
        return KEYER_PADDLE_IAMBIC_B;
    }

    return mode;
}

static int keyer_cycle_int(int value, int count, int direction)
{
    int next = value;

    if (direction < 0) {
        --next;
    } else if (direction > 0) {
        ++next;
    }

    if (next < 0) {
        next = count - 1;
    } else if (next >= count) {
        next = 0;
    }

    return next;
}

static uint16_t keyer_wpm_to_dit_ms(uint16_t wpm)
{
    uint16_t clamped = keyer_clamp_tx_wpm(wpm);
    uint16_t dit_ms = (uint16_t)(1200U / clamped);
    return dit_ms > 0U ? dit_ms : 1U;
}

static uint16_t keyer_dit_ms(void)
{
    return keyer_wpm_to_dit_ms(s_key_in_wpm);
}

static uint8_t keyer_unit_ms_to_wpm(uint32_t unit_ms)
{
    uint32_t wpm;

    if (unit_ms == 0U) {
        unit_ms = 1U;
    }

    wpm = (1200U + (unit_ms / 2U)) / unit_ms;
    return (uint8_t)keyer_clamp_tx_wpm((uint16_t)wpm);
}

static uint16_t keyer_sk_dit_ms(void)
{
    if (s_sk_unit_ms == 0U) {
        s_sk_unit_ms = keyer_wpm_to_dit_ms(s_config.sk_wpm);
    }

    return s_sk_unit_ms;
}

static void keyer_reset_sk_adaptive_state(void)
{
    s_config.sk_wpm = (uint8_t)keyer_clamp_tx_wpm(s_config.sk_wpm);
    s_sk_wpm_current = s_config.sk_wpm;
    s_sk_unit_ms = keyer_wpm_to_dit_ms(s_config.sk_wpm);
    s_sk_save_candidate_active = false;
    s_sk_save_candidate_wpm = 0U;
    s_sk_save_candidate_active_ms = 0U;
    s_sk_save_candidate_last_tick = 0;
    s_sk_wpm_save_requested = false;
}

static uint16_t keyer_duration_ms_from_ticks(TickType_t start, TickType_t end)
{
    uint32_t elapsed_ticks = (uint32_t)(end - start);
    uint32_t elapsed_ms = elapsed_ticks * (uint32_t)portTICK_PERIOD_MS;

    if (elapsed_ms == 0U) {
        elapsed_ms = 1U;
    }

    if (elapsed_ms > UINT16_MAX) {
        return UINT16_MAX;
    }

    return (uint16_t)elapsed_ms;
}

static uint8_t keyer_u8_delta(uint8_t a, uint8_t b)
{
    return a > b ? (uint8_t)(a - b) : (uint8_t)(b - a);
}

static TickType_t keyer_ms_to_delay_ticks(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return ticks > 0 ? ticks : 1;
}

static bool keyer_tick_reached(TickType_t now, TickType_t target)
{
    return (int32_t)(now - target) >= 0;
}

static uint8_t keyer_clamp_u8(uint8_t value, uint8_t min_value, uint8_t max_value)
{
    if (value < min_value) {
        return min_value;
    }

    if (value > max_value) {
        return max_value;
    }

    return value;
}

static bool keyer_op_candidate_char(char ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '/';
}

static bool keyer_op_text_has_digit(const char *text, size_t len)
{
    if (text == NULL) {
        return false;
    }

    for (size_t i = 0U; i < len; ++i) {
        if (text[i] >= '0' && text[i] <= '9') {
            return true;
        }
    }

    return false;
}

static void keyer_copy_mycall(char *destination, const char *source)
{
    size_t out = 0U;

    if (destination == NULL) {
        return;
    }

    if (source == NULL) {
        destination[0] = '\0';
        return;
    }

    while (*source != '\0' && out < KEYER_MYCALL_MAX_LEN) {
        char ch = (char)toupper((unsigned char)*source++);
        if (keyer_op_candidate_char(ch)) {
            destination[out++] = ch;
        }
    }
    destination[out] = '\0';
}

static void keyer_op_reset_candidate(void)
{
    s_op_candidate[0] = '\0';
    s_op_candidate_len = 0U;
    s_op_candidate_has_digit = false;
    s_op_candidate_overflow = false;
}

static void keyer_op_reset_stream(void)
{
    keyer_op_reset_candidate();
    s_op_rolling[0] = '\0';
}

static bool keyer_op_build_base_call(const char *candidate, char *base, size_t base_size)
{
    size_t len;
    size_t segment_start = 0U;

    if (candidate == NULL || base == NULL || base_size == 0U) {
        return false;
    }

    base[0] = '\0';
    len = strlen(candidate);
    if (len == 0U || !keyer_op_text_has_digit(candidate, len)) {
        return false;
    }

    for (size_t i = 0U; i <= len; ++i) {
        if (candidate[i] == '/' || candidate[i] == '\0') {
            size_t segment_len = i - segment_start;
            if (segment_len > 0U && segment_len <= KEYER_OP_CALL_MAX_LEN &&
                keyer_op_text_has_digit(&candidate[segment_start], segment_len)) {
                snprintf(base, base_size, "%.*s", (int)segment_len, &candidate[segment_start]);
                return true;
            }
            segment_start = i + 1U;
        }
    }

    return false;
}

static bool keyer_op_is_mycall(const char *candidate, const char *base_call)
{
    if (s_config.mycall[0] == '\0') {
        return false;
    }

    return (candidate != NULL && strcmp(candidate, s_config.mycall) == 0) ||
           (base_call != NULL && strcmp(base_call, s_config.mycall) == 0);
}

static bool keyer_op_lookup_name(const char *base_call, char *name, size_t name_size)
{
    if (base_call == NULL || name == NULL || name_size == 0U) {
        return false;
    }

    name[0] = '\0';
    for (size_t i = 0U; i < s_op_entry_count; ++i) {
        if (strcmp(s_op_entries[i].call, base_call) == 0) {
            snprintf(name, name_size, "%s", s_op_entries[i].name);
            return true;
        }
    }

    return false;
}

static void keyer_op_try_candidate(void)
{
    char base_call[KEYER_OP_CALL_MAX_LEN + 1U];
    char name[KEYER_OP_NAME_MAX_LEN + 1U];

    if (s_op_candidate_len == 0U || !s_op_candidate_has_digit || s_op_candidate_overflow ||
        !keyer_op_build_base_call(s_op_candidate, base_call, sizeof(base_call)) ||
        keyer_op_is_mycall(s_op_candidate, base_call)) {
        return;
    }

    if (keyer_op_lookup_name(base_call, name, sizeof(name))) {
        snprintf(s_op_name, sizeof(s_op_name), "%s", name);
        ESP_LOGI(TAG, "OP match: %s -> %s", base_call, s_op_name);
    }
}

static void keyer_op_feed_candidate_char(char ch)
{
    if (keyer_op_candidate_char(ch)) {
        if (s_op_candidate_len >= KEYER_OP_CANDIDATE_MAX_LEN) {
            s_op_candidate_overflow = true;
            return;
        }

        s_op_candidate[s_op_candidate_len++] = ch;
        s_op_candidate[s_op_candidate_len] = '\0';
        if (ch >= '0' && ch <= '9') {
            s_op_candidate_has_digit = true;
        }
        keyer_op_try_candidate();
        return;
    }

    keyer_op_try_candidate();
    keyer_op_reset_candidate();
}

static void keyer_op_clear_display(void)
{
    if (s_op_name[0] != '\0') {
        ESP_LOGI(TAG, "OP display cleared");
    }
    s_op_name[0] = '\0';
}

static bool keyer_op_feed_rolling_char(char ch)
{
    char normalized = (char)toupper((unsigned char)ch);
    size_t len = strlen(s_op_rolling);
    char padded[KEYER_OP_ROLLING_LEN + 3U];

    if (!isalnum((unsigned char)normalized)) {
        normalized = ' ';
    }

    if (len >= KEYER_OP_ROLLING_LEN) {
        memmove(s_op_rolling, s_op_rolling + 1U, KEYER_OP_ROLLING_LEN - 1U);
        len = KEYER_OP_ROLLING_LEN - 1U;
    }
    s_op_rolling[len++] = normalized;
    s_op_rolling[len] = '\0';

    snprintf(padded, sizeof(padded), " %s ", s_op_rolling);
    if (strstr(padded, " 73 ") != NULL || strstr(padded, " 7 3 ") != NULL ||
        strstr(padded, " 72 ") != NULL || strstr(padded, " 7 2 ") != NULL) {
        keyer_op_clear_display();
        keyer_op_reset_stream();
        return true;
    }

    return false;
}

static void keyer_set_gpio_level_if_ready(gpio_num_t gpio, bool active)
{
    if (!s_key_out_gpio_ready) {
        return;
    }

    gpio_set_level(gpio, active ? KEYER_GPIO_ACTIVE_LEVEL : KEYER_GPIO_INACTIVE_LEVEL);
}

static void keyer_apply_key_out_idle(void)
{
    if (s_key_out_mode == KEYER_KEY_OUT_SK_M) {
        keyer_set_gpio_level_if_ready(KEYER_GPIO_OUT_TIP, false);
        keyer_set_gpio_level_if_ready(KEYER_GPIO_OUT_RING, true);
        return;
    }

    keyer_set_gpio_level_if_ready(KEYER_GPIO_OUT_TIP, false);
    keyer_set_gpio_level_if_ready(KEYER_GPIO_OUT_RING, false);
}

static void keyer_set_key_out_lines(bool tip_active, bool ring_active)
{
    if (s_key_out_mode == KEYER_KEY_OUT_OFF) {
        tip_active = false;
        ring_active = false;
    }

    if (s_key_out_mode == KEYER_KEY_OUT_SK_M) {
        ring_active = true;
    }

    keyer_set_gpio_level_if_ready(KEYER_GPIO_OUT_TIP, tip_active);
    keyer_set_gpio_level_if_ready(KEYER_GPIO_OUT_RING, ring_active);
}

static void keyer_release_key_out_element(void)
{
    s_key_out_element_active = false;
    s_key_out_release_tick = 0;
    keyer_apply_key_out_idle();
}

static void keyer_start_key_out_element(keyer_element_t element, uint32_t duration_ms)
{
    bool tip_active = false;
    bool ring_active = false;

    if (element == KEYER_ELEMENT_NONE || duration_ms == 0U) {
        return;
    }

    switch (s_key_out_mode) {
    case KEYER_KEY_OUT_PADDLE:
        tip_active = element == KEYER_ELEMENT_DIT;
        ring_active = element == KEYER_ELEMENT_DAH;
        break;
    case KEYER_KEY_OUT_PADDLE_R:
        tip_active = element == KEYER_ELEMENT_DAH;
        ring_active = element == KEYER_ELEMENT_DIT;
        break;
    case KEYER_KEY_OUT_SK:
        tip_active = true;
        ring_active = true;
        break;
    case KEYER_KEY_OUT_SK_M:
        tip_active = true;
        ring_active = true;
        break;
    case KEYER_KEY_OUT_OFF:
    default:
        tip_active = false;
        ring_active = false;
        break;
    }

    keyer_set_key_out_lines(tip_active, ring_active);
    s_key_out_element_active = true;
    s_key_out_release_tick = xTaskGetTickCount() + keyer_ms_to_delay_ticks(duration_ms);
}

static void keyer_set_key_out_straight(bool key_down)
{
    bool tip_active = false;
    bool ring_active = false;

    if (!key_down) {
        keyer_apply_key_out_idle();
        return;
    }

    switch (s_key_out_mode) {
    case KEYER_KEY_OUT_PADDLE:
    case KEYER_KEY_OUT_PADDLE_R:
    case KEYER_KEY_OUT_SK:
        tip_active = true;
        ring_active = true;
        break;
    case KEYER_KEY_OUT_SK_M:
        tip_active = true;
        ring_active = true;
        break;
    case KEYER_KEY_OUT_OFF:
    default:
        break;
    }

    keyer_set_key_out_lines(tip_active, ring_active);
}

static void keyer_update_key_out_timing(TickType_t now)
{
    if (s_key_out_element_active && keyer_tick_reached(now, s_key_out_release_tick)) {
        keyer_release_key_out_element();
    }
}

static void keyer_clear_events(void)
{
    s_event_head = 0U;
    s_event_tail = 0U;
    s_event_count = 0U;
}

static void keyer_push_event(keyer_event_type_t type, char decoded_char, uint16_t duration_ms)
{
    keyer_event_t event = {
        .type = type,
        .decoded_char = decoded_char,
        .duration_ms = duration_ms,
    };

    if (type == KEYER_EVENT_NONE) {
        return;
    }

    if (s_event_count >= KEYER_EVENT_RING_CAP) {
        ESP_LOGW(TAG, "keyer event ring full; dropping event type=%d", type);
        return;
    }

    s_event_ring[s_event_tail] = event;
    s_event_tail = (uint8_t)((s_event_tail + 1U) % KEYER_EVENT_RING_CAP);
    ++s_event_count;
}

static bool keyer_tx_playback_active(void)
{
    return s_tx_state != KEYER_TX_IDLE;
}

static void keyer_tx_mark_changed(void)
{
    ++s_tx_revision;
}

static bool keyer_tx_has_remaining_text(void)
{
    return s_tx_text_pos < s_tx_text_len;
}

static void keyer_tx_compact(void)
{
    uint16_t remaining;

    if (s_tx_text_pos == 0U) {
        return;
    }

    if (s_tx_text_pos >= s_tx_text_len) {
        s_tx_text[0] = '\0';
        s_tx_text_len = 0U;
        s_tx_text_pos = 0U;
        return;
    }

    remaining = (uint16_t)(s_tx_text_len - s_tx_text_pos);
    memmove(s_tx_text, &s_tx_text[s_tx_text_pos], remaining);
    s_tx_text[remaining] = '\0';
    s_tx_text_len = remaining;
    s_tx_text_pos = 0U;
}

static uint16_t keyer_tx_protected_len(void)
{
    if (s_tx_state == KEYER_TX_ELEMENT ||
        (s_tx_state == KEYER_TX_GAP && s_tx_pattern != NULL)) {
        return (uint16_t)(s_tx_text_pos + 1U);
    }

    return s_tx_text_pos;
}

static bool keyer_tx_tail_is_space(void)
{
    if (s_tx_text_len == 0U) {
        return false;
    }

    return s_tx_text[s_tx_text_len - 1U] == ' ';
}

static void keyer_filter_cancel_ignored_inputs(bool *tip_pressed, bool *ring_pressed)
{
    if (tip_pressed != NULL && s_cancel_ignore_tip) {
        if (*tip_pressed) {
            *tip_pressed = false;
        } else {
            s_cancel_ignore_tip = false;
        }
    }

    if (ring_pressed != NULL && s_cancel_ignore_ring) {
        if (*ring_pressed) {
            *ring_pressed = false;
        } else {
            s_cancel_ignore_ring = false;
        }
    }
}

static void keyer_ignore_physical_paddle_until_release(keyer_element_t element,
                                                       keyer_key_in_mode_t mode,
                                                       bool tip_pressed,
                                                       bool ring_pressed)
{
    bool dit_on_tip = mode != KEYER_KEY_IN_PADDLE_R;

    if (element == KEYER_ELEMENT_DIT) {
        if (dit_on_tip && tip_pressed) {
            s_cancel_ignore_tip = true;
        } else if (!dit_on_tip && ring_pressed) {
            s_cancel_ignore_ring = true;
        }
    } else if (element == KEYER_ELEMENT_DAH) {
        if (dit_on_tip && ring_pressed) {
            s_cancel_ignore_ring = true;
        } else if (!dit_on_tip && tip_pressed) {
            s_cancel_ignore_tip = true;
        }
    }
}

static bool keyer_cancel_tx_with_paddle_element(keyer_element_t element,
                                                keyer_key_in_mode_t mode,
                                                bool tip_pressed,
                                                bool ring_pressed)
{
    if ((!keyer_tx_playback_active() && !keyer_tx_has_remaining_text()) ||
        element == KEYER_ELEMENT_NONE) {
        return false;
    }

    /* The physical element that cancels active text TX is consumed here: no keyOut,
     * sidetone, or decoder append is produced for this press. */
    keyer_stop_tx_playback(keyer_tx_playback_active());
    keyer_ignore_physical_paddle_until_release(element, mode, tip_pressed, ring_pressed);

    keyer_push_event(KEYER_EVENT_TX_CANCELLED, '\0', 0U);
    return true;
}

static bool keyer_cancel_tx_with_straight_key(bool tip_source)
{
    if (!keyer_tx_playback_active() && !keyer_tx_has_remaining_text()) {
        return false;
    }

    /* Straight-key cancel is consumed until the physical key is released. */
    keyer_stop_tx_playback(keyer_tx_playback_active());
    if (tip_source) {
        s_cancel_ignore_tip = true;
    } else {
        s_cancel_ignore_ring = true;
    }
    keyer_push_event(KEYER_EVENT_TX_CANCELLED, '\0', 0U);
    return true;
}

static void keyer_reset_paddle_decoder_state(void)
{
    keyer_decoder_reset(&s_paddle_decoder);
    s_decoder_last_element_end_tick = 0;
    s_decoder_char_finalized = false;
    s_decoder_space_emitted = false;
}

static keyer_element_t keyer_opposite_element(keyer_element_t element)
{
    switch (element) {
    case KEYER_ELEMENT_DIT:
        return KEYER_ELEMENT_DAH;
    case KEYER_ELEMENT_DAH:
        return KEYER_ELEMENT_DIT;
    case KEYER_ELEMENT_NONE:
    default:
        return KEYER_ELEMENT_DIT;
    }
}

static bool keyer_gpio_pressed(gpio_num_t gpio)
{
    return gpio_get_level(gpio) == KEYER_GPIO_ACTIVE_LEVEL;
}

static void keyer_read_paddle_inputs(bool *tip_pressed, bool *ring_pressed)
{
    bool tip = false;
    bool ring = false;

    if (s_gpio_ready) {
        tip = keyer_gpio_pressed(KEYER_GPIO_TIP);
        ring = keyer_gpio_pressed(KEYER_GPIO_RING);
    }

    if (tip_pressed != NULL) {
        *tip_pressed = tip;
    }

    if (ring_pressed != NULL) {
        *ring_pressed = ring;
    }
}

static void keyer_stop_straight_tone(void)
{
    if (!s_straight_tone_on) {
        return;
    }

    if (!s_mute) {
        audio_service_tone_off();
    }
    s_straight_tone_on = false;
}

static void keyer_start_tune_output(void)
{
    if (s_tune_output_active) {
        return;
    }

    keyer_set_key_out_straight(true);
    if (!s_mute) {
        audio_service_tone_on();
        s_straight_tone_on = true;
    }
    s_tune_output_active = true;
}

static void keyer_stop_tune_output(void)
{
    if (!s_tune_output_active) {
        return;
    }

    keyer_stop_straight_tone();
    keyer_set_key_out_straight(false);
    s_tune_output_active = false;
}

static void keyer_clear_tune_state(void)
{
    s_tune_latched = false;
    s_tune_consume_until_release = false;
    keyer_stop_tune_output();
}

static void keyer_reset_straight_state(void)
{
    keyer_stop_straight_tone();
    keyer_set_key_out_straight(false);
    s_straight_key_down = false;
    s_straight_key_down_tick = 0;
    keyer_reset_sk_adaptive_state();
}

static void keyer_clear_iambic_state(void)
{
    s_paddle_state = KEYER_PADDLE_IDLE;
    s_paddle_ready_tick = 0;
    s_paddle_element_end_tick = 0;
    s_last_element = KEYER_ELEMENT_NONE;
    s_dit_memory = false;
    s_dah_memory = false;
    s_squeeze_latched = false;
    s_mode_b_extra_pending = false;
    s_bug_dah_down = false;
}

static void keyer_reset_input_state(void)
{
    keyer_reset_straight_state();
    keyer_clear_iambic_state();
    keyer_reset_paddle_decoder_state();
    keyer_clear_events();
    keyer_release_key_out_element();
    s_cancel_ignore_tip = false;
    s_cancel_ignore_ring = false;
}

static void keyer_configure_gpio(void)
{
    gpio_config_t config = {
        .pin_bit_mask = (1ULL << KEYER_GPIO_TIP) | (1ULL << KEYER_GPIO_RING),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        s_gpio_ready = false;
        ESP_LOGE(TAG, "failed to configure paddle GPIO: %s", esp_err_to_name(err));
        return;
    }

    s_gpio_ready = true;
    ESP_LOGI(TAG, "paddle GPIO ready: tip=G13 ring=G15 active=low pullups=enabled");
}

static void keyer_configure_key_out_gpio(void)
{
    gpio_config_t config = {
        .pin_bit_mask = (1ULL << KEYER_GPIO_OUT_TIP) | (1ULL << KEYER_GPIO_OUT_RING),
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        s_key_out_gpio_ready = false;
        ESP_LOGE(TAG, "failed to configure keyOut GPIO: %s", esp_err_to_name(err));
        return;
    }

    s_key_out_gpio_ready = true;
    keyer_apply_key_out_idle();
    ESP_LOGI(TAG, "keyOut GPIO ready: tip=G3 ring=G6 active=low open-drain");
}

static void keyer_start_paddle_element(keyer_element_t element)
{
    uint16_t unit_ms = keyer_dit_ms();
    bool dit = element == KEYER_ELEMENT_DIT;
    uint32_t tone_ms = dit ? unit_ms : (3U * unit_ms);
    TickType_t now = xTaskGetTickCount();

    if (element == KEYER_ELEMENT_NONE) {
        return;
    }

    keyer_start_key_out_element(element, tone_ms);

    if (dit) {
        keyer_push_event(KEYER_EVENT_DIT, '.', unit_ms);
        if (!s_mute) {
            audio_service_play_dit(unit_ms);
        }
        s_dit_memory = false;
    } else {
        keyer_push_event(KEYER_EVENT_DAH, '-', (uint16_t)tone_ms);
        if (!s_mute) {
            audio_service_play_dah(unit_ms);
        }
        s_dah_memory = false;
    }

    s_last_element = element;
    s_paddle_state = KEYER_PADDLE_WAIT_GAP;
    s_paddle_element_end_tick = now + keyer_ms_to_delay_ticks(tone_ms);
    s_paddle_ready_tick = s_paddle_element_end_tick + keyer_ms_to_delay_ticks(unit_ms);

    keyer_decoder_append(&s_paddle_decoder, !dit);
    s_decoder_last_element_end_tick = s_paddle_element_end_tick;
    s_decoder_char_finalized = false;
    s_decoder_space_emitted = false;
}

static void keyer_emit_decoder_result(keyer_decoder_result_t result)
{
    switch (result.type) {
    case KEYER_DECODER_RESULT_CHAR:
        keyer_push_event(KEYER_EVENT_CHAR_COMPLETE, result.ch, 0U);
        s_decoder_char_finalized = true;
        s_decoder_space_emitted = false;
        break;
    case KEYER_DECODER_RESULT_BACKSPACE:
        keyer_push_event(KEYER_EVENT_BACKSPACE, '\b', 0U);
        s_decoder_char_finalized = false;
        s_decoder_space_emitted = true;
        break;
    case KEYER_DECODER_RESULT_ENTER:
        keyer_push_event(KEYER_EVENT_ENTER, '\n', 0U);
        s_decoder_char_finalized = false;
        s_decoder_space_emitted = true;
        break;
    case KEYER_DECODER_RESULT_SPACE:
        keyer_push_event(KEYER_EVENT_WORD_SPACE, ' ', 0U);
        s_decoder_char_finalized = false;
        s_decoder_space_emitted = true;
        break;
    case KEYER_DECODER_RESULT_INVALID:
        ESP_LOGW(TAG, "invalid Morse pattern ignored");
        s_decoder_char_finalized = false;
        s_decoder_space_emitted = true;
        break;
    case KEYER_DECODER_RESULT_NONE:
    default:
        break;
    }
}

static void keyer_update_decode_gaps(TickType_t now, uint16_t unit_ms)
{
    TickType_t char_due;
    TickType_t word_due;

    if (s_decoder_last_element_end_tick == 0) {
        return;
    }

    if (keyer_decoder_has_pending(&s_paddle_decoder)) {
        char_due = s_decoder_last_element_end_tick + keyer_ms_to_delay_ticks(3U * unit_ms);
        if (!keyer_tick_reached(now, char_due)) {
            return;
        }

        keyer_emit_decoder_result(keyer_decoder_finalize(&s_paddle_decoder));
    }

    if (!s_decoder_char_finalized || s_decoder_space_emitted) {
        return;
    }

    word_due = s_decoder_last_element_end_tick + keyer_ms_to_delay_ticks(7U * unit_ms);
    if (keyer_tick_reached(now, word_due)) {
        keyer_push_event(KEYER_EVENT_WORD_SPACE, ' ', 0U);
        s_decoder_space_emitted = true;
    }
}

static void keyer_update_iambic_memory(bool dit_pressed,
                                       bool dah_pressed,
                                       TickType_t now)
{
    bool both_pressed = dit_pressed && dah_pressed;

    if (s_paddle_state != KEYER_PADDLE_WAIT_GAP) {
        return;
    }

    if (both_pressed) {
        s_squeeze_latched = true;
    }

    if (dit_pressed && s_last_element == KEYER_ELEMENT_DAH) {
        s_dit_memory = true;
    }

    if (dah_pressed && s_last_element == KEYER_ELEMENT_DIT) {
        s_dah_memory = true;
    }

    if (s_paddle_mode == KEYER_PADDLE_IAMBIC_B && !both_pressed && !dit_pressed &&
        !dah_pressed && s_squeeze_latched &&
        !s_mode_b_extra_pending &&
        !keyer_tick_reached(now, s_paddle_element_end_tick)) {
        s_mode_b_extra_pending = true;
    }
}

static keyer_element_t keyer_choose_next_iambic_element(bool dit_pressed, bool dah_pressed)
{
    if (s_mode_b_extra_pending) {
        s_mode_b_extra_pending = false;
        s_squeeze_latched = false;
        return keyer_opposite_element(s_last_element);
    }

    if (dit_pressed && dah_pressed) {
        s_squeeze_latched = true;
        return keyer_opposite_element(s_last_element);
    }

    if (s_dit_memory) {
        return KEYER_ELEMENT_DIT;
    }

    if (s_dah_memory) {
        return KEYER_ELEMENT_DAH;
    }

    if (dit_pressed) {
        return KEYER_ELEMENT_DIT;
    }

    if (dah_pressed) {
        return KEYER_ELEMENT_DAH;
    }

    return KEYER_ELEMENT_NONE;
}

static void keyer_update_bug_mode(bool dit_pressed,
                                  bool dah_pressed,
                                  bool tip_pressed,
                                  bool ring_pressed)
{
    TickType_t now = xTaskGetTickCount();

    s_squeeze_latched = false;
    s_mode_b_extra_pending = false;

    if (dah_pressed) {
        if (!s_bug_dah_down) {
            if (keyer_cancel_tx_with_paddle_element(KEYER_ELEMENT_DAH,
                                                    s_key_in_mode,
                                                    tip_pressed,
                                                    ring_pressed)) {
                return;
            }
            keyer_clear_iambic_state();
            keyer_set_key_out_straight(true);
            keyer_push_event(KEYER_EVENT_DAH, '-', (uint16_t)(3U * keyer_dit_ms()));
            if (!s_mute) {
                audio_service_tone_on();
            }
            s_straight_tone_on = !s_mute;
            s_bug_dah_down = true;
            keyer_decoder_append(&s_paddle_decoder, true);
        }
        s_decoder_last_element_end_tick = now;
        s_decoder_char_finalized = false;
        s_decoder_space_emitted = false;
        return;
    }

    if (s_bug_dah_down) {
        keyer_stop_straight_tone();
        keyer_set_key_out_straight(false);
        s_bug_dah_down = false;
        s_decoder_last_element_end_tick = now;
    }

    if (s_paddle_state == KEYER_PADDLE_WAIT_GAP) {
        if (!keyer_tick_reached(now, s_paddle_ready_tick) || audio_service_is_busy()) {
            return;
        }

        s_paddle_state = KEYER_PADDLE_IDLE;
    }

    keyer_update_decode_gaps(now, keyer_dit_ms());

    if (dit_pressed) {
        if (keyer_cancel_tx_with_paddle_element(KEYER_ELEMENT_DIT,
                                                s_key_in_mode,
                                                tip_pressed,
                                                ring_pressed)) {
            return;
        }
        keyer_start_paddle_element(KEYER_ELEMENT_DIT);
        return;
    }

    keyer_clear_iambic_state();
}

static void keyer_update_tune_mode(bool tip_pressed, bool ring_pressed)
{
    bool any_pressed = tip_pressed || ring_pressed;

    keyer_clear_iambic_state();
    s_straight_key_down = false;

    if (s_tune_consume_until_release) {
        if (any_pressed) {
            keyer_stop_tune_output();
            return;
        }

        s_tune_consume_until_release = false;
    }

    if (s_tune_latched) {
        if (any_pressed) {
            s_tune_latched = false;
            s_tune_consume_until_release = true;
            keyer_stop_tune_output();
            return;
        }

        keyer_start_tune_output();
        return;
    }

    if (any_pressed) {
        keyer_start_tune_output();
    } else {
        keyer_stop_tune_output();
    }
}

static void keyer_update_paddle_mode(keyer_key_in_mode_t mode, bool tip_pressed, bool ring_pressed)
{
    bool dit_pressed;
    bool dah_pressed;
    TickType_t now = xTaskGetTickCount();
    keyer_element_t next;

    if (mode == KEYER_KEY_IN_PADDLE_R) {
        dit_pressed = ring_pressed;
        dah_pressed = tip_pressed;
    } else {
        dit_pressed = tip_pressed;
        dah_pressed = ring_pressed;
    }

    if (s_paddle_mode == KEYER_PADDLE_BUG) {
        keyer_update_bug_mode(dit_pressed, dah_pressed, tip_pressed, ring_pressed);
        return;
    }

    keyer_stop_straight_tone();

    keyer_update_iambic_memory(dit_pressed, dah_pressed, now);

    if (s_paddle_state == KEYER_PADDLE_WAIT_GAP) {
        if (!keyer_tick_reached(now, s_paddle_ready_tick) || audio_service_is_busy()) {
            return;
        }

        s_paddle_state = KEYER_PADDLE_IDLE;
    }

    keyer_update_decode_gaps(now, keyer_dit_ms());

    next = keyer_choose_next_iambic_element(dit_pressed, dah_pressed);
    if (next != KEYER_ELEMENT_NONE) {
        if (keyer_cancel_tx_with_paddle_element(next, mode, tip_pressed, ring_pressed)) {
            return;
        }
        keyer_start_paddle_element(next);
        return;
    }

    keyer_clear_iambic_state();
}

static void keyer_track_sk_wpm_stability(uint8_t observed_wpm,
                                         uint16_t duration_ms,
                                         TickType_t now)
{
    uint32_t active_ms = duration_ms;

    if (observed_wpm == s_config.sk_wpm) {
        s_sk_save_candidate_active = false;
        s_sk_save_candidate_active_ms = 0U;
        s_sk_save_candidate_last_tick = 0;
        return;
    }

    if (!s_sk_save_candidate_active ||
        keyer_u8_delta(observed_wpm, s_sk_save_candidate_wpm) > KEYER_SK_STABLE_BAND_WPM) {
        s_sk_save_candidate_active = true;
        s_sk_save_candidate_wpm = observed_wpm;
        s_sk_save_candidate_active_ms = duration_ms;
        s_sk_save_candidate_last_tick = now;
        return;
    }

    if (s_sk_save_candidate_last_tick != 0) {
        uint32_t elapsed_ms = keyer_duration_ms_from_ticks(s_sk_save_candidate_last_tick, now);
        uint32_t gap_ms = elapsed_ms > duration_ms ? elapsed_ms - duration_ms : 0U;

        if (gap_ms <= KEYER_SK_ACTIVE_GAP_MAX_MS) {
            active_ms += gap_ms;
        }
    }

    if (KEYER_SK_WPM_SAVE_STABLE_MS - s_sk_save_candidate_active_ms <= active_ms) {
        s_config.sk_wpm = s_sk_save_candidate_wpm;
        s_sk_wpm_save_requested = true;
        s_sk_save_candidate_active = false;
        s_sk_save_candidate_active_ms = 0U;
        s_sk_save_candidate_last_tick = 0;
        ESP_LOGI(TAG, "straight key wpm save requested: %u", (unsigned)s_config.sk_wpm);
        return;
    }

    s_sk_save_candidate_active_ms += active_ms;
    s_sk_save_candidate_last_tick = now;
}

static void keyer_adapt_sk_timing(keyer_element_t element, uint16_t duration_ms, TickType_t now)
{
    uint32_t sample_unit_ms;
    uint32_t adapted_unit_ms;
    uint16_t min_unit_ms = keyer_wpm_to_dit_ms(KEYER_MAX_TX_WPM);
    uint16_t max_unit_ms = keyer_wpm_to_dit_ms(KEYER_MIN_TX_WPM);

    if (element == KEYER_ELEMENT_DAH) {
        sample_unit_ms = ((uint32_t)duration_ms + 1U) / 3U;
    } else {
        sample_unit_ms = duration_ms;
    }

    if (sample_unit_ms < min_unit_ms) {
        sample_unit_ms = min_unit_ms;
    } else if (sample_unit_ms > max_unit_ms) {
        sample_unit_ms = max_unit_ms;
    }

    if (s_sk_unit_ms == 0U) {
        s_sk_unit_ms = (uint16_t)sample_unit_ms;
    } else {
        adapted_unit_ms =
            ((uint32_t)s_sk_unit_ms * KEYER_SK_ADAPT_OLD_WEIGHT + sample_unit_ms +
             (KEYER_SK_ADAPT_DENOMINATOR / 2U)) /
            KEYER_SK_ADAPT_DENOMINATOR;
        s_sk_unit_ms = (uint16_t)adapted_unit_ms;
    }

    s_sk_wpm_current = keyer_unit_ms_to_wpm(s_sk_unit_ms);
    keyer_track_sk_wpm_stability(s_sk_wpm_current, duration_ms, now);
}

static void keyer_finish_straight_element(TickType_t now)
{
    uint16_t duration_ms = keyer_duration_ms_from_ticks(s_straight_key_down_tick, now);
    uint16_t unit_ms = keyer_sk_dit_ms();
    keyer_element_t element =
        (uint32_t)duration_ms <= (uint32_t)unit_ms * 2U ? KEYER_ELEMENT_DIT : KEYER_ELEMENT_DAH;
    bool dah = element == KEYER_ELEMENT_DAH;

    keyer_push_event(dah ? KEYER_EVENT_DAH : KEYER_EVENT_DIT, dah ? '-' : '.', duration_ms);
    keyer_decoder_append(&s_paddle_decoder, dah);
    s_decoder_last_element_end_tick = now;
    s_decoder_char_finalized = false;
    s_decoder_space_emitted = false;
    keyer_adapt_sk_timing(element, duration_ms, now);
}

static void keyer_update_straight_key_mode(bool key_down, bool tip_source)
{
    TickType_t now = xTaskGetTickCount();

    keyer_clear_iambic_state();

    if (key_down && !s_straight_key_down) {
        if (keyer_cancel_tx_with_straight_key(tip_source)) {
            return;
        }
        keyer_set_key_out_straight(true);
        if (!s_mute) {
            audio_service_tone_on();
            s_straight_tone_on = true;
        }
        s_straight_key_down = true;
        s_straight_key_down_tick = now;
    } else if (!key_down && s_straight_key_down) {
        keyer_stop_straight_tone();
        keyer_set_key_out_straight(false);
        s_straight_key_down = false;
        keyer_finish_straight_element(now);
    }

    if (!key_down) {
        keyer_update_decode_gaps(now, keyer_sk_dit_ms());
    }
}

static void keyer_stop_tx_playback(bool stop_audio)
{
    bool had_text = keyer_tx_has_remaining_text() || s_tx_text_len > 0U ||
                    keyer_tx_playback_active();

    s_tx_text[0] = '\0';
    s_tx_text_len = 0U;
    s_tx_text_pos = 0U;
    s_tx_pattern = NULL;
    s_tx_pattern_pos = 0U;
    s_tx_state = KEYER_TX_IDLE;
    s_tx_due_tick = 0;
    keyer_release_key_out_element();
    if (stop_audio) {
        audio_service_stop_all();
    }
    if (had_text) {
        keyer_tx_mark_changed();
    }
}

static bool keyer_tx_next_supported_char(char *out_ch, const char **out_pattern)
{
    while (s_tx_text_pos < s_tx_text_len) {
        char ch = (char)toupper((unsigned char)s_tx_text[s_tx_text_pos]);

        if (ch == ' ') {
            if (out_ch != NULL) {
                *out_ch = ch;
            }
            if (out_pattern != NULL) {
                *out_pattern = NULL;
            }
            return true;
        }

        const char *pattern = audio_service_get_cw_pattern(ch);
        if (pattern != NULL) {
            if (out_ch != NULL) {
                *out_ch = ch;
            }
            if (out_pattern != NULL) {
                *out_pattern = pattern;
            }
            return true;
        }

        ESP_LOGW(TAG, "unsupported TX character skipped: '%c'", s_tx_text[s_tx_text_pos]);
        ++s_tx_text_pos;
        keyer_tx_mark_changed();
    }

    return false;
}

static void keyer_tx_schedule_gap(uint32_t units)
{
    uint32_t ms = (uint32_t)keyer_dit_ms() * units;
    s_tx_state = KEYER_TX_GAP;
    s_tx_due_tick = xTaskGetTickCount() + keyer_ms_to_delay_ticks(ms);
}

static void keyer_tx_start_current_symbol(void)
{
    char symbol;
    uint32_t units;
    uint16_t unit_ms = keyer_dit_ms();
    keyer_element_t element;

    if (s_tx_pattern == NULL || s_tx_pattern[s_tx_pattern_pos] == '\0') {
        return;
    }

    symbol = s_tx_pattern[s_tx_pattern_pos];
    units = symbol == '-' ? 3U : 1U;
    element = symbol == '-' ? KEYER_ELEMENT_DAH : KEYER_ELEMENT_DIT;

    keyer_start_key_out_element(element, (uint32_t)unit_ms * units);
    if (!s_mute) {
        if (element == KEYER_ELEMENT_DAH) {
            audio_service_play_dah(unit_ms);
        } else {
            audio_service_play_dit(unit_ms);
        }
    }

    s_tx_state = KEYER_TX_ELEMENT;
    s_tx_due_tick = xTaskGetTickCount() + keyer_ms_to_delay_ticks((uint32_t)unit_ms * units);
}

static void keyer_tx_begin_next_char(void)
{
    char ch;
    const char *pattern;

    if (!keyer_tx_next_supported_char(&ch, &pattern)) {
        keyer_stop_tx_playback(false);
        return;
    }

    if (ch == ' ') {
        while (s_tx_text_pos < s_tx_text_len && s_tx_text[s_tx_text_pos] == ' ') {
            ++s_tx_text_pos;
        }
        keyer_tx_mark_changed();
        keyer_tx_schedule_gap(7U);
        return;
    }

    s_tx_pattern = pattern;
    s_tx_pattern_pos = 0U;
    keyer_tx_start_current_symbol();
}

static void keyer_tx_advance_after_element(void)
{
    char next_ch;
    const char *next_pattern;

    keyer_release_key_out_element();
    ++s_tx_pattern_pos;
    if (s_tx_pattern != NULL && s_tx_pattern[s_tx_pattern_pos] != '\0') {
        keyer_tx_schedule_gap(1U);
        return;
    }

    ++s_tx_text_pos;
    keyer_tx_mark_changed();
    s_tx_pattern = NULL;
    s_tx_pattern_pos = 0U;

    if (!keyer_tx_next_supported_char(&next_ch, &next_pattern)) {
        keyer_stop_tx_playback(false);
        return;
    }

    if (next_ch == ' ') {
        while (s_tx_text_pos < s_tx_text_len && s_tx_text[s_tx_text_pos] == ' ') {
            ++s_tx_text_pos;
        }
        keyer_tx_mark_changed();
        keyer_tx_schedule_gap(7U);
    } else {
        keyer_tx_schedule_gap(3U);
    }
}

static void keyer_update_tx(TickType_t now)
{
    if (s_tx_state == KEYER_TX_IDLE) {
        return;
    }

    if (!keyer_tick_reached(now, s_tx_due_tick)) {
        return;
    }

    if (s_tx_state == KEYER_TX_ELEMENT) {
        keyer_tx_advance_after_element();
        return;
    }

    if (s_tx_state == KEYER_TX_GAP) {
        if (s_tx_pattern != NULL && s_tx_pattern[s_tx_pattern_pos] != '\0') {
            keyer_tx_start_current_symbol();
        } else {
            keyer_tx_begin_next_char();
        }
    }
}

void keyer_service_init(void)
{
    keyer_configure_gpio();
    keyer_configure_key_out_gpio();
    keyer_reset_input_state();
    ESP_LOGI(TAG, "initialized paddle/key owner");
    ESP_LOGI(TAG,
             "key in=%s key out=%s paddle=%s wpm=%u",
             keyer_service_key_in_mode_label(s_key_in_mode),
             keyer_service_key_out_mode_label(s_key_out_mode),
             keyer_service_paddle_mode_label(s_paddle_mode),
             (unsigned)s_key_in_wpm);
}

keyer_key_in_mode_t keyer_service_get_key_in_mode(void)
{
    return s_key_in_mode;
}

void keyer_service_set_key_in_mode(keyer_key_in_mode_t mode)
{
    keyer_key_in_mode_t next = keyer_clamp_key_in_mode(mode);

    if (s_key_in_mode != next) {
        keyer_reset_input_state();
    }

    s_key_in_mode = next;
    ESP_LOGI(TAG, "key in mode: %s", keyer_service_key_in_mode_label(s_key_in_mode));
}

void keyer_service_cycle_key_in_mode(int direction)
{
    keyer_service_set_key_in_mode(
        (keyer_key_in_mode_t)keyer_cycle_int((int)s_key_in_mode, KEYER_KEY_IN_MODE_COUNT, direction));
}

keyer_key_out_mode_t keyer_service_get_key_out_mode(void)
{
    return s_key_out_mode;
}

void keyer_service_set_key_out_mode(keyer_key_out_mode_t mode)
{
    s_key_out_mode = keyer_clamp_key_out_mode(mode);
    s_config.key_out_mode = s_key_out_mode;
    keyer_release_key_out_element();
    ESP_LOGI(TAG, "key out mode: %s", keyer_service_key_out_mode_label(s_key_out_mode));
}

void keyer_service_cycle_key_out_mode(int direction)
{
    keyer_service_set_key_out_mode((keyer_key_out_mode_t)keyer_cycle_int(
        (int)s_key_out_mode, KEYER_KEY_OUT_MODE_COUNT, direction));
}

uint8_t keyer_service_get_key_in_wpm(void)
{
    return s_key_in_wpm;
}

void keyer_service_set_key_in_wpm(uint8_t wpm)
{
    s_key_in_wpm = (uint8_t)keyer_clamp_tx_wpm(wpm);
    ESP_LOGI(TAG, "key in wpm: %u", (unsigned)s_key_in_wpm);
}

void keyer_service_adjust_key_in_wpm(int delta)
{
    int next = (int)s_key_in_wpm + delta;
    if (next < (int)KEYER_MIN_TX_WPM) {
        next = (int)KEYER_MIN_TX_WPM;
    } else if (next > (int)KEYER_MAX_TX_WPM) {
        next = (int)KEYER_MAX_TX_WPM;
    }

    keyer_service_set_key_in_wpm((uint8_t)next);
}

uint8_t keyer_service_get_sk_wpm(void)
{
    if (s_sk_wpm_current == 0U) {
        return s_config.sk_wpm;
    }

    return s_sk_wpm_current;
}

void keyer_service_set_sk_wpm(uint8_t wpm)
{
    s_config.sk_wpm = (uint8_t)keyer_clamp_tx_wpm(wpm);
    keyer_reset_sk_adaptive_state();
    keyer_reset_paddle_decoder_state();
    ESP_LOGI(TAG, "straight key wpm: %u", (unsigned)s_config.sk_wpm);
}

void keyer_service_adjust_sk_wpm(int delta)
{
    int next = (int)s_config.sk_wpm + delta;
    if (next < (int)KEYER_MIN_TX_WPM) {
        next = (int)KEYER_MIN_TX_WPM;
    } else if (next > (int)KEYER_MAX_TX_WPM) {
        next = (int)KEYER_MAX_TX_WPM;
    }

    keyer_service_set_sk_wpm((uint8_t)next);
}

keyer_paddle_mode_t keyer_service_get_paddle_mode(void)
{
    return s_paddle_mode;
}

void keyer_service_set_paddle_mode(keyer_paddle_mode_t mode)
{
    s_paddle_mode = keyer_clamp_paddle_mode(mode);
    s_config.paddle_mode = s_paddle_mode;
    keyer_clear_iambic_state();
    ESP_LOGI(TAG, "paddle mode: %s", keyer_service_paddle_mode_label(s_paddle_mode));
}

void keyer_service_cycle_paddle_mode(int direction)
{
    keyer_service_set_paddle_mode((keyer_paddle_mode_t)keyer_cycle_int(
        (int)s_paddle_mode, KEYER_PADDLE_MODE_COUNT, direction));
}

uint8_t keyer_service_get_tx_delay_s(void)
{
    return s_config.tx_delay_s;
}

void keyer_service_set_tx_delay_s(uint8_t delay_s)
{
    s_config.tx_delay_s = keyer_clamp_u8(delay_s, KEYER_TX_DELAY_MIN_S, KEYER_TX_DELAY_MAX_S);
    ESP_LOGI(TAG, "tx delay: %u s", (unsigned)s_config.tx_delay_s);
}

uint8_t keyer_service_get_tune_timeout_s(void)
{
    return s_config.tune_timeout_s;
}

void keyer_service_set_tune_timeout_s(uint8_t timeout_s)
{
    s_config.tune_timeout_s =
        keyer_clamp_u8(timeout_s, KEYER_TUNE_TIMEOUT_MIN_S, KEYER_TUNE_TIMEOUT_MAX_S);
    ESP_LOGI(TAG, "tune timeout: %u s", (unsigned)s_config.tune_timeout_s);
}

uint8_t keyer_service_get_repeat_interval_s(void)
{
    return s_config.repeat_interval_s;
}

void keyer_service_set_repeat_interval_s(uint8_t interval_s)
{
    s_config.repeat_interval_s =
        keyer_clamp_u8(interval_s, KEYER_REPEAT_MIN_S, KEYER_REPEAT_MAX_S);
    ESP_LOGI(TAG, "repeat interval: %u s", (unsigned)s_config.repeat_interval_s);
}

const char *keyer_service_get_message(uint8_t index)
{
    if (index >= KEYER_MESSAGE_COUNT) {
        return "";
    }

    return s_config.message[index];
}

void keyer_service_set_message(uint8_t index, const char *message)
{
    if (index >= KEYER_MESSAGE_COUNT) {
        return;
    }

    snprintf(s_config.message[index], sizeof(s_config.message[index]), "%s", message ? message : "");
    ESP_LOGI(TAG, "message M%u: %s", (unsigned)(index + 1U), s_config.message[index]);
}

const char *keyer_service_get_mycall(void)
{
    return s_config.mycall;
}

void keyer_service_set_mycall(const char *mycall)
{
    keyer_copy_mycall(s_config.mycall, mycall);
    ESP_LOGI(TAG, "mycall: %s", s_config.mycall);
}

const keyer_config_t *keyer_service_get_config(void)
{
    return &s_config;
}

void keyer_service_get_config_copy(keyer_config_t *config)
{
    if (config == NULL) {
        return;
    }

    *config = s_config;
}

void keyer_service_set_config(const keyer_config_t *config)
{
    if (config == NULL) {
        return;
    }

    s_config = *config;
    s_config.key_out_mode = keyer_clamp_key_out_mode(s_config.key_out_mode);
    s_config.paddle_mode = keyer_clamp_paddle_mode(s_config.paddle_mode);
    s_config.sk_wpm = (uint8_t)keyer_clamp_tx_wpm(s_config.sk_wpm);
    s_config.tx_delay_s =
        keyer_clamp_u8(s_config.tx_delay_s, KEYER_TX_DELAY_MIN_S, KEYER_TX_DELAY_MAX_S);
    s_config.tune_timeout_s =
        keyer_clamp_u8(s_config.tune_timeout_s,
                       KEYER_TUNE_TIMEOUT_MIN_S,
                       KEYER_TUNE_TIMEOUT_MAX_S);
    s_config.repeat_interval_s =
        keyer_clamp_u8(s_config.repeat_interval_s, KEYER_REPEAT_MIN_S, KEYER_REPEAT_MAX_S);
    s_config.mycall[KEYER_MYCALL_MAX_LEN] = '\0';
    keyer_copy_mycall(s_config.mycall, s_config.mycall);
    for (uint8_t i = 0U; i < KEYER_MESSAGE_COUNT; ++i) {
        s_config.message[i][KEYER_MESSAGE_MAX_LEN] = '\0';
    }

    keyer_service_set_key_out_mode(s_config.key_out_mode);
    keyer_service_set_paddle_mode(s_config.paddle_mode);
    keyer_service_set_sk_wpm(s_config.sk_wpm);
}

bool keyer_service_get_mute(void)
{
    return s_mute;
}

void keyer_service_set_mute(bool mute)
{
    s_mute = mute;
    if (s_mute) {
        audio_service_tone_off();
        s_straight_tone_on = false;
    } else if (s_tune_output_active && !s_straight_tone_on) {
        audio_service_tone_on();
        s_straight_tone_on = true;
    }
    ESP_LOGI(TAG, "mute: %s", s_mute ? "on" : "off");
}

void keyer_service_toggle_mute(void)
{
    keyer_service_set_mute(!s_mute);
}

const char *keyer_service_key_in_mode_label(keyer_key_in_mode_t mode)
{
    switch (mode) {
    case KEYER_KEY_IN_PADDLE:
        return "Pdl";
    case KEYER_KEY_IN_PADDLE_R:
        return "Pdl-R";
    case KEYER_KEY_IN_SK_T:
        return "SK-T";
    case KEYER_KEY_IN_SK_R:
        return "SK-R";
    default:
        return "Unknown";
    }
}

const char *keyer_service_key_out_mode_label(keyer_key_out_mode_t mode)
{
    switch (mode) {
    case KEYER_KEY_OUT_PADDLE:
        return "Pdl";
    case KEYER_KEY_OUT_PADDLE_R:
        return "Pdl-R";
    case KEYER_KEY_OUT_SK:
        return "SK";
    case KEYER_KEY_OUT_SK_M:
        return "SK-M";
    case KEYER_KEY_OUT_OFF:
        return "OFF";
    default:
        return "Unknown";
    }
}

const char *keyer_service_paddle_mode_label(keyer_paddle_mode_t mode)
{
    switch (mode) {
    case KEYER_PADDLE_IAMBIC_A:
        return "IambicA";
    case KEYER_PADDLE_IAMBIC_B:
        return "IambicB";
    case KEYER_PADDLE_BUG:
        return "Bug";
    default:
        return "Unknown";
    }
}

void keyer_service_set_input_mode(keyer_input_mode_t mode)
{
    switch (mode) {
    case KEYER_INPUT_STRAIGHT_KEY:
        keyer_service_set_key_in_mode(KEYER_KEY_IN_SK_T);
        break;
    case KEYER_INPUT_SINGLE_PADDLE:
    case KEYER_INPUT_DUAL_PADDLE:
        keyer_service_set_key_in_mode(KEYER_KEY_IN_PADDLE);
        break;
    default:
        ESP_LOGW(TAG, "unknown legacy input mode: %s", keyer_input_mode_name(mode));
        keyer_service_set_key_in_mode(KEYER_KEY_IN_PADDLE);
        break;
    }
}

uint16_t keyer_service_get_tx_wpm(void)
{
    return s_key_in_wpm;
}

void keyer_service_set_tx_wpm(uint16_t wpm)
{
    keyer_service_set_key_in_wpm((uint8_t)keyer_clamp_tx_wpm(wpm));
}

void keyer_service_adjust_tx_wpm(int delta)
{
    keyer_service_adjust_key_in_wpm(delta);
}

uint16_t keyer_service_get_dit_ms(void)
{
    return keyer_dit_ms();
}

bool keyer_service_tx_append_text(const char *text, bool insert_space)
{
    size_t text_len;
    bool add_space = false;

    if (text == NULL || text[0] == '\0') {
        return true;
    }

    text_len = strlen(text);
    keyer_tx_compact();

    if (insert_space && s_tx_text_len > 0U && !keyer_tx_tail_is_space() && text[0] != ' ') {
        add_space = true;
    }

    if ((size_t)s_tx_text_len + text_len + (add_space ? 1U : 0U) > KEYER_TX_TEXT_MAX) {
        return false;
    }

    if (add_space) {
        s_tx_text[s_tx_text_len++] = ' ';
    }

    memcpy(&s_tx_text[s_tx_text_len], text, text_len);
    s_tx_text_len = (uint16_t)(s_tx_text_len + text_len);
    s_tx_text[s_tx_text_len] = '\0';
    keyer_tx_mark_changed();
    return true;
}

bool keyer_service_tx_backspace(void)
{
    uint16_t protected_len;

    keyer_tx_compact();
    protected_len = keyer_tx_protected_len();

    if (s_tx_text_len == 0U || s_tx_text_len <= protected_len) {
        return false;
    }

    --s_tx_text_len;
    s_tx_text[s_tx_text_len] = '\0';
    keyer_tx_mark_changed();
    return true;
}

void keyer_service_tx_clear(void)
{
    keyer_stop_tx_playback(keyer_tx_playback_active());
}

void keyer_service_tx_start(void)
{
    if (keyer_tx_playback_active() || !keyer_tx_has_remaining_text()) {
        return;
    }

    keyer_tx_compact();
    if (!keyer_tx_has_remaining_text()) {
        return;
    }

    s_tx_pattern = NULL;
    s_tx_pattern_pos = 0U;
    s_tx_state = KEYER_TX_GAP;
    s_tx_due_tick = xTaskGetTickCount();
    ESP_LOGI(TAG, "TX FIFO start: %s", s_tx_text);
}

bool keyer_service_tx_has_text(void)
{
    return keyer_tx_has_remaining_text();
}

void keyer_service_tx_copy_text(char *destination, size_t destination_size)
{
    size_t remaining;
    size_t copy_len;
    const char *source;

    if (destination == NULL || destination_size == 0U) {
        return;
    }

    destination[0] = '\0';
    if (!keyer_tx_has_remaining_text()) {
        return;
    }

    remaining = (size_t)(s_tx_text_len - s_tx_text_pos);
    copy_len = remaining;
    source = &s_tx_text[s_tx_text_pos];

    if (copy_len >= destination_size) {
        copy_len = destination_size - 1U;
        source += remaining - copy_len;
    }

    memcpy(destination, source, copy_len);
    destination[copy_len] = '\0';
}

uint32_t keyer_service_tx_revision(void)
{
    return s_tx_revision;
}

void keyer_service_set_op_table(keyer_op_entry_t *entries, size_t count)
{
    if (entries == NULL) {
        count = 0U;
    }

    free(s_op_entries);
    s_op_entries = entries;
    s_op_entry_count = count;
    ESP_LOGI(TAG, "OP lookup table loaded: %u entries", (unsigned)s_op_entry_count);
}

void keyer_service_op_feed_char(char ch)
{
    char normalized = (char)toupper((unsigned char)ch);

    if (keyer_op_feed_rolling_char(normalized)) {
        return;
    }
    keyer_op_feed_candidate_char(normalized);
}

void keyer_service_op_feed_text(const char *text)
{
    if (text == NULL) {
        return;
    }

    while (*text != '\0') {
        keyer_service_op_feed_char(*text++);
    }
}

const char *keyer_service_get_op_name(void)
{
    return s_op_name;
}

void keyer_service_clear_op_name(void)
{
    keyer_op_clear_display();
    keyer_op_reset_stream();
}

void keyer_service_play_text(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }

    keyer_service_tx_clear();
    (void)keyer_service_tx_append_text(text, false);
    keyer_service_tx_start();
}

void keyer_service_stop_tx(void)
{
    keyer_service_tx_clear();
}

bool keyer_service_is_tx_active(void)
{
    return keyer_tx_playback_active();
}

void keyer_service_set_tune_active(bool active)
{
    if (!active) {
        keyer_clear_tune_state();
        s_tune_active = false;
        keyer_reset_input_state();
        ESP_LOGI(TAG, "tune mode: off");
        return;
    }

    keyer_service_tx_clear();
    keyer_service_clear_op_name();

    keyer_reset_input_state();
    keyer_clear_tune_state();
    keyer_clear_events();
    s_tune_active = true;
    ESP_LOGI(TAG, "tune mode: on");
}

bool keyer_service_get_tune_active(void)
{
    return s_tune_active;
}

void keyer_service_set_tune_latched(bool latched)
{
    if (!s_tune_active) {
        latched = false;
    }

    s_tune_latched = latched;
    s_tune_consume_until_release = false;

    if (s_tune_latched) {
        keyer_start_tune_output();
    } else {
        keyer_stop_tune_output();
    }

    ESP_LOGI(TAG, "tune latch: %s", s_tune_latched ? "on" : "off");
}

bool keyer_service_get_tune_latched(void)
{
    return s_tune_latched;
}

bool keyer_service_get_tune_output_active(void)
{
    return s_tune_output_active;
}

bool keyer_service_take_sk_wpm_save_request(void)
{
    bool requested = s_sk_wpm_save_requested;
    s_sk_wpm_save_requested = false;
    return requested;
}

void keyer_service_update(void)
{
    bool tip_pressed;
    bool ring_pressed;
    TickType_t now = xTaskGetTickCount();

    keyer_update_key_out_timing(now);
    keyer_update_tx(now);
    keyer_read_paddle_inputs(&tip_pressed, &ring_pressed);
    keyer_filter_cancel_ignored_inputs(&tip_pressed, &ring_pressed);

    if (s_tune_active) {
        keyer_update_tune_mode(tip_pressed, ring_pressed);
        return;
    }

    switch (s_key_in_mode) {
    case KEYER_KEY_IN_PADDLE:
    case KEYER_KEY_IN_PADDLE_R:
        keyer_update_paddle_mode(s_key_in_mode, tip_pressed, ring_pressed);
        break;
    case KEYER_KEY_IN_SK_T:
        keyer_update_straight_key_mode(tip_pressed, true);
        break;
    case KEYER_KEY_IN_SK_R:
        keyer_update_straight_key_mode(ring_pressed, false);
        break;
    default:
        keyer_reset_input_state();
        break;
    }
}

keyer_event_t keyer_service_poll_event(void)
{
    keyer_event_t event;

    if (s_event_count == 0U) {
        return KEYER_NO_EVENT;
    }

    event = s_event_ring[s_event_head];
    s_event_head = (uint8_t)((s_event_head + 1U) % KEYER_EVENT_RING_CAP);
    --s_event_count;
    return event;
}

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
#define KEYER_TX_TEXT_MAX 255U
#define KEYER_TX_DELAY_MIN_S 1U
#define KEYER_TX_DELAY_MAX_S 99U
#define KEYER_REPEAT_MIN_S 1U
#define KEYER_REPEAT_MAX_S 99U

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
static bool s_gpio_ready;
static bool s_key_out_gpio_ready;
static bool s_straight_tone_on;
static bool s_straight_key_down;
static bool s_mute;
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
static uint16_t s_tx_text_pos;
static const char *s_tx_pattern;
static uint8_t s_tx_pattern_pos;
static keyer_tx_state_t s_tx_state = KEYER_TX_IDLE;
static TickType_t s_tx_due_tick;

static keyer_config_t s_config = {
    .key_out_mode = KEYER_KEY_OUT_PADDLE,
    .paddle_mode = KEYER_PADDLE_IAMBIC_B,
    .tx_delay_s = 1U,
    .repeat_interval_s = 6U,
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

static uint16_t keyer_dit_ms(void)
{
    return (uint16_t)(1200U / keyer_clamp_tx_wpm(s_key_in_wpm));
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

static void keyer_reset_straight_state(void)
{
    keyer_stop_straight_tone();
    keyer_set_key_out_straight(false);
    s_straight_key_down = false;
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

    keyer_stop_tx_playback(true);
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
        ESP_LOGW(TAG, "invalid paddle Morse pattern ignored");
        s_decoder_char_finalized = false;
        s_decoder_space_emitted = true;
        break;
    case KEYER_DECODER_RESULT_NONE:
    default:
        break;
    }
}

static void keyer_update_paddle_decode_gaps(TickType_t now)
{
    uint16_t unit_ms = keyer_dit_ms();
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

static void keyer_update_bug_mode(bool dit_pressed, bool dah_pressed)
{
    TickType_t now = xTaskGetTickCount();

    s_squeeze_latched = false;
    s_mode_b_extra_pending = false;

    if (dah_pressed) {
        if (!s_bug_dah_down) {
            keyer_stop_tx_playback(true);
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

    keyer_update_paddle_decode_gaps(now);

    if (dit_pressed) {
        keyer_start_paddle_element(KEYER_ELEMENT_DIT);
        return;
    }

    keyer_clear_iambic_state();
}

static void keyer_update_paddle_mode(keyer_key_in_mode_t mode, bool tip_pressed, bool ring_pressed)
{
    bool dit_pressed;
    bool dah_pressed;
    TickType_t now = xTaskGetTickCount();
    keyer_element_t next;

    keyer_stop_straight_tone();

    if (mode == KEYER_KEY_IN_PADDLE_R) {
        dit_pressed = ring_pressed;
        dah_pressed = tip_pressed;
    } else {
        dit_pressed = tip_pressed;
        dah_pressed = ring_pressed;
    }

    if (s_paddle_mode == KEYER_PADDLE_BUG) {
        keyer_update_bug_mode(dit_pressed, dah_pressed);
        return;
    }

    keyer_update_iambic_memory(dit_pressed, dah_pressed, now);

    if (s_paddle_state == KEYER_PADDLE_WAIT_GAP) {
        if (!keyer_tick_reached(now, s_paddle_ready_tick) || audio_service_is_busy()) {
            return;
        }

        s_paddle_state = KEYER_PADDLE_IDLE;
    }

    keyer_update_paddle_decode_gaps(now);

    next = keyer_choose_next_iambic_element(dit_pressed, dah_pressed);
    if (next != KEYER_ELEMENT_NONE) {
        keyer_start_paddle_element(next);
        return;
    }

    keyer_clear_iambic_state();
}

static void keyer_update_straight_key_mode(bool key_down)
{
    keyer_clear_iambic_state();

    if (key_down && !s_straight_key_down) {
        keyer_stop_tx_playback(true);
        keyer_set_key_out_straight(true);
        keyer_push_event(KEYER_EVENT_DIT, '.', 0U);
        if (!s_mute) {
            audio_service_tone_on();
            s_straight_tone_on = true;
        }
        s_straight_key_down = true;
    } else if (!key_down && s_straight_key_down) {
        keyer_stop_straight_tone();
        keyer_set_key_out_straight(false);
        s_straight_key_down = false;
    }
}

static void keyer_stop_tx_playback(bool stop_audio)
{
    s_tx_text[0] = '\0';
    s_tx_text_pos = 0U;
    s_tx_pattern = NULL;
    s_tx_pattern_pos = 0U;
    s_tx_state = KEYER_TX_IDLE;
    s_tx_due_tick = 0;
    keyer_release_key_out_element();
    if (stop_audio) {
        audio_service_stop_all();
    }
}

static bool keyer_tx_next_supported_char(char *out_ch, const char **out_pattern)
{
    while (s_tx_text[s_tx_text_pos] != '\0') {
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
        while (s_tx_text[s_tx_text_pos] == ' ') {
            ++s_tx_text_pos;
        }
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
    s_tx_pattern = NULL;
    s_tx_pattern_pos = 0U;

    if (!keyer_tx_next_supported_char(&next_ch, &next_pattern)) {
        keyer_stop_tx_playback(false);
        return;
    }

    if (next_ch == ' ') {
        while (s_tx_text[s_tx_text_pos] == ' ') {
            ++s_tx_text_pos;
        }
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
    s_config.tx_delay_s =
        keyer_clamp_u8(s_config.tx_delay_s, KEYER_TX_DELAY_MIN_S, KEYER_TX_DELAY_MAX_S);
    s_config.repeat_interval_s =
        keyer_clamp_u8(s_config.repeat_interval_s, KEYER_REPEAT_MIN_S, KEYER_REPEAT_MAX_S);
    for (uint8_t i = 0U; i < KEYER_MESSAGE_COUNT; ++i) {
        s_config.message[i][KEYER_MESSAGE_MAX_LEN] = '\0';
    }

    keyer_service_set_key_out_mode(s_config.key_out_mode);
    keyer_service_set_paddle_mode(s_config.paddle_mode);
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

void keyer_service_play_text(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }

    keyer_stop_tx_playback(true);
    snprintf(s_tx_text, sizeof(s_tx_text), "%s", text);
    s_tx_text_pos = 0U;
    s_tx_pattern = NULL;
    s_tx_pattern_pos = 0U;
    s_tx_state = KEYER_TX_GAP;
    s_tx_due_tick = xTaskGetTickCount();
    ESP_LOGI(TAG, "TX text: %s", s_tx_text);
}

void keyer_service_stop_tx(void)
{
    keyer_stop_tx_playback(true);
}

bool keyer_service_is_tx_active(void)
{
    return s_tx_state != KEYER_TX_IDLE;
}

void keyer_service_update(void)
{
    bool tip_pressed;
    bool ring_pressed;
    TickType_t now = xTaskGetTickCount();

    keyer_update_key_out_timing(now);
    keyer_update_tx(now);
    keyer_read_paddle_inputs(&tip_pressed, &ring_pressed);

    switch (s_key_in_mode) {
    case KEYER_KEY_IN_PADDLE:
    case KEYER_KEY_IN_PADDLE_R:
        keyer_update_paddle_mode(s_key_in_mode, tip_pressed, ring_pressed);
        break;
    case KEYER_KEY_IN_SK_T:
        keyer_update_straight_key_mode(tip_pressed);
        break;
    case KEYER_KEY_IN_SK_R:
        keyer_update_straight_key_mode(ring_pressed);
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

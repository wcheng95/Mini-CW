/*
 * keyer_service
 *
 * Responsibility: Owns paddle/key input abstraction and KeyIn timing.
 * Hardware ownership: paddle/key GPIO. Sidetone is requested through
 * audio_service, not produced here.
 */

#include "keyer_service.h"

#include "audio_service.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>

static const char *TAG = "keyer_service";

#define KEYER_DEFAULT_TX_WPM 20U
#define KEYER_MIN_TX_WPM 5U
#define KEYER_MAX_TX_WPM 60U
#define KEYER_IO_MODE_COUNT 4
#define KEYER_GPIO_TIP GPIO_NUM_13
#define KEYER_GPIO_RING GPIO_NUM_15
#define KEYER_GPIO_ACTIVE_LEVEL 0

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

static keyer_io_mode_t s_key_in_mode = KEYER_IO_PADDLE;
static keyer_io_mode_t s_key_out_mode = KEYER_IO_PADDLE;
static uint8_t s_key_in_wpm = KEYER_DEFAULT_TX_WPM;
static uint8_t s_key_out_wpm = KEYER_DEFAULT_TX_WPM;
static bool s_gpio_ready;
static bool s_straight_tone_on;
static keyer_paddle_state_t s_paddle_state = KEYER_PADDLE_IDLE;
static TickType_t s_paddle_ready_tick;
static TickType_t s_paddle_element_end_tick;
static keyer_element_t s_last_element = KEYER_ELEMENT_NONE;
static bool s_dit_memory;
static bool s_dah_memory;
static bool s_squeeze_latched;
static bool s_mode_b_extra_pending;

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

static keyer_io_mode_t keyer_clamp_io_mode(keyer_io_mode_t mode)
{
    if ((int)mode < 0 || (int)mode >= KEYER_IO_MODE_COUNT) {
        return KEYER_IO_PADDLE;
    }

    return mode;
}

static keyer_io_mode_t keyer_cycle_io_mode(keyer_io_mode_t mode, int direction)
{
    int next = (int)keyer_clamp_io_mode(mode);

    if (direction < 0) {
        --next;
    } else if (direction > 0) {
        ++next;
    }

    if (next < 0) {
        next = KEYER_IO_MODE_COUNT - 1;
    } else if (next >= KEYER_IO_MODE_COUNT) {
        next = 0;
    }

    return (keyer_io_mode_t)next;
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

    audio_service_tone_off();
    s_straight_tone_on = false;
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
}

static void keyer_reset_input_state(void)
{
    keyer_stop_straight_tone();
    keyer_clear_iambic_state();
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

static void keyer_start_paddle_element(keyer_element_t element)
{
    uint16_t unit_ms = keyer_dit_ms();
    bool dit = element == KEYER_ELEMENT_DIT;
    uint32_t tone_ms = dit ? unit_ms : (3U * unit_ms);
    TickType_t now = xTaskGetTickCount();

    if (element == KEYER_ELEMENT_NONE) {
        return;
    }

    if (dit) {
        audio_service_play_dit(unit_ms);
        s_dit_memory = false;
    } else {
        audio_service_play_dah(unit_ms);
        s_dah_memory = false;
    }

    s_last_element = element;
    s_paddle_state = KEYER_PADDLE_WAIT_GAP;
    s_paddle_element_end_tick = now + keyer_ms_to_delay_ticks(tone_ms);
    s_paddle_ready_tick = s_paddle_element_end_tick + keyer_ms_to_delay_ticks(unit_ms);
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

    if (!both_pressed && !dit_pressed && !dah_pressed && s_squeeze_latched &&
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

static void keyer_update_paddle_mode(keyer_io_mode_t mode, bool tip_pressed, bool ring_pressed)
{
    bool dit_pressed;
    bool dah_pressed;
    TickType_t now = xTaskGetTickCount();
    keyer_element_t next;

    keyer_stop_straight_tone();

    if (mode == KEYER_IO_PADDLE_R) {
        dit_pressed = ring_pressed;
        dah_pressed = tip_pressed;
    } else {
        dit_pressed = tip_pressed;
        dah_pressed = ring_pressed;
    }

    keyer_update_iambic_memory(dit_pressed, dah_pressed, now);

    if (s_paddle_state == KEYER_PADDLE_WAIT_GAP) {
        if (!keyer_tick_reached(now, s_paddle_ready_tick) || audio_service_is_busy()) {
            return;
        }

        s_paddle_state = KEYER_PADDLE_IDLE;
    }

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

    if (key_down && !s_straight_tone_on) {
        audio_service_tone_on();
        s_straight_tone_on = true;
    } else if (!key_down && s_straight_tone_on) {
        keyer_stop_straight_tone();
    }
}

void keyer_service_init(void)
{
    keyer_configure_gpio();
    keyer_reset_input_state();
    ESP_LOGI(TAG, "initialized paddle/key owner");
    ESP_LOGI(TAG,
             "key in=%s key out=%s key_in_wpm=%u key_out_wpm=%u",
             keyer_service_io_mode_label(s_key_in_mode),
             keyer_service_io_mode_label(s_key_out_mode),
             (unsigned)s_key_in_wpm,
             (unsigned)s_key_out_wpm);
}

keyer_io_mode_t keyer_service_get_key_in_mode(void)
{
    return s_key_in_mode;
}

void keyer_service_set_key_in_mode(keyer_io_mode_t mode)
{
    keyer_io_mode_t next = keyer_clamp_io_mode(mode);

    if (s_key_in_mode != next) {
        keyer_reset_input_state();
    }

    s_key_in_mode = next;
    ESP_LOGI(TAG, "key in mode: %s", keyer_service_io_mode_label(s_key_in_mode));
}

void keyer_service_cycle_key_in_mode(int direction)
{
    keyer_service_set_key_in_mode(keyer_cycle_io_mode(s_key_in_mode, direction));
}

keyer_io_mode_t keyer_service_get_key_out_mode(void)
{
    return s_key_out_mode;
}

void keyer_service_set_key_out_mode(keyer_io_mode_t mode)
{
    s_key_out_mode = keyer_clamp_io_mode(mode);
    ESP_LOGI(TAG, "key out mode: %s", keyer_service_io_mode_label(s_key_out_mode));
}

void keyer_service_cycle_key_out_mode(int direction)
{
    keyer_service_set_key_out_mode(keyer_cycle_io_mode(s_key_out_mode, direction));
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

uint8_t keyer_service_get_key_out_wpm(void)
{
    return s_key_out_wpm;
}

void keyer_service_set_key_out_wpm(uint8_t wpm)
{
    s_key_out_wpm = (uint8_t)keyer_clamp_tx_wpm(wpm);
    ESP_LOGI(TAG, "key out wpm: %u", (unsigned)s_key_out_wpm);
}

void keyer_service_adjust_key_out_wpm(int delta)
{
    int next = (int)s_key_out_wpm + delta;
    if (next < (int)KEYER_MIN_TX_WPM) {
        next = (int)KEYER_MIN_TX_WPM;
    } else if (next > (int)KEYER_MAX_TX_WPM) {
        next = (int)KEYER_MAX_TX_WPM;
    }

    keyer_service_set_key_out_wpm((uint8_t)next);
}

const char *keyer_service_io_mode_label(keyer_io_mode_t mode)
{
    switch (mode) {
    case KEYER_IO_PADDLE:
        return "Paddle";
    case KEYER_IO_PADDLE_R:
        return "PaddleR";
    case KEYER_IO_SK:
        return "SK";
    case KEYER_IO_SK_MONO:
        return "SK-Mono";
    default:
        return "Unknown";
    }
}

void keyer_service_set_input_mode(keyer_input_mode_t mode)
{
    switch (mode) {
    case KEYER_INPUT_STRAIGHT_KEY:
        keyer_service_set_key_in_mode(KEYER_IO_SK);
        break;
    case KEYER_INPUT_SINGLE_PADDLE:
    case KEYER_INPUT_DUAL_PADDLE:
        keyer_service_set_key_in_mode(KEYER_IO_PADDLE);
        break;
    default:
        ESP_LOGW(TAG, "unknown legacy input mode: %s", keyer_input_mode_name(mode));
        keyer_service_set_key_in_mode(KEYER_IO_PADDLE);
        break;
    }
}

uint16_t keyer_service_get_tx_wpm(void)
{
    /*
     * TX speed belongs to keyer_service. Step 3 exposes adjustment controls,
     * but paddle timing and full keyer logic still come later.
     */
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

void keyer_service_update(void)
{
    bool tip_pressed;
    bool ring_pressed;

    keyer_read_paddle_inputs(&tip_pressed, &ring_pressed);

    switch (s_key_in_mode) {
    case KEYER_IO_PADDLE:
    case KEYER_IO_PADDLE_R:
        keyer_update_paddle_mode(s_key_in_mode, tip_pressed, ring_pressed);
        break;
    case KEYER_IO_SK:
        keyer_update_straight_key_mode(tip_pressed);
        break;
    case KEYER_IO_SK_MONO:
        keyer_update_straight_key_mode(tip_pressed || ring_pressed);
        break;
    default:
        keyer_reset_input_state();
        break;
    }
}

keyer_event_t keyer_service_poll_event(void)
{
    return KEYER_NO_EVENT;
}

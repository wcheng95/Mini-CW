/*
 * keyer_service
 *
 * Responsibility: Owns paddle/key input abstraction for straight key, single
 * paddle, and dual paddle future support.
 * Hardware ownership: paddle/key GPIO or its HAL owner. Other modules consume
 * keyer events and must not read raw key/paddle GPIO.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KEYER_INPUT_STRAIGHT_KEY = 0,
    KEYER_INPUT_SINGLE_PADDLE,
    KEYER_INPUT_DUAL_PADDLE,
} keyer_input_mode_t;

#define KEYER_MESSAGE_COUNT 5U
#define KEYER_MESSAGE_MAX_LEN 95U

typedef enum {
    KEYER_KEY_IN_PADDLE = 0,
    KEYER_KEY_IN_PADDLE_R,
    KEYER_KEY_IN_SK_T,
    KEYER_KEY_IN_SK_R,
} keyer_key_in_mode_t;

typedef enum {
    KEYER_KEY_OUT_PADDLE = 0,
    KEYER_KEY_OUT_PADDLE_R,
    KEYER_KEY_OUT_SK,
    KEYER_KEY_OUT_SK_M,
    KEYER_KEY_OUT_OFF,
} keyer_key_out_mode_t;

typedef enum {
    KEYER_PADDLE_IAMBIC_A = 0,
    KEYER_PADDLE_IAMBIC_B,
    KEYER_PADDLE_BUG,
} keyer_paddle_mode_t;

typedef struct {
    keyer_key_out_mode_t key_out_mode;
    keyer_paddle_mode_t paddle_mode;
    uint8_t tx_delay_s;
    uint8_t tune_timeout_s;
    uint8_t repeat_interval_s;
    char message[KEYER_MESSAGE_COUNT][KEYER_MESSAGE_MAX_LEN + 1U];
} keyer_config_t;

typedef enum {
    KEYER_EVENT_NONE = 0,
    KEYER_EVENT_DIT,
    KEYER_EVENT_DAH,
    KEYER_EVENT_CHAR_COMPLETE,
    KEYER_EVENT_BACKSPACE,
    KEYER_EVENT_ENTER,
    KEYER_EVENT_WORD_SPACE,
    KEYER_EVENT_TIMING_WARNING,
    KEYER_EVENT_TIMING_ERROR,
} keyer_event_type_t;

typedef struct {
    keyer_event_type_t type;
    char decoded_char;
    uint16_t duration_ms;
} keyer_event_t;

void keyer_service_init(void);
keyer_key_in_mode_t keyer_service_get_key_in_mode(void);
void keyer_service_set_key_in_mode(keyer_key_in_mode_t mode);
void keyer_service_cycle_key_in_mode(int direction);
keyer_key_out_mode_t keyer_service_get_key_out_mode(void);
void keyer_service_set_key_out_mode(keyer_key_out_mode_t mode);
void keyer_service_cycle_key_out_mode(int direction);
uint8_t keyer_service_get_key_in_wpm(void);
void keyer_service_set_key_in_wpm(uint8_t wpm);
void keyer_service_adjust_key_in_wpm(int delta);
keyer_paddle_mode_t keyer_service_get_paddle_mode(void);
void keyer_service_set_paddle_mode(keyer_paddle_mode_t mode);
void keyer_service_cycle_paddle_mode(int direction);
uint8_t keyer_service_get_tx_delay_s(void);
void keyer_service_set_tx_delay_s(uint8_t delay_s);
uint8_t keyer_service_get_tune_timeout_s(void);
void keyer_service_set_tune_timeout_s(uint8_t timeout_s);
uint8_t keyer_service_get_repeat_interval_s(void);
void keyer_service_set_repeat_interval_s(uint8_t interval_s);
const char *keyer_service_get_message(uint8_t index);
void keyer_service_set_message(uint8_t index, const char *message);
const keyer_config_t *keyer_service_get_config(void);
void keyer_service_set_config(const keyer_config_t *config);
void keyer_service_get_config_copy(keyer_config_t *config);
bool keyer_service_get_mute(void);
void keyer_service_set_mute(bool mute);
void keyer_service_toggle_mute(void);
const char *keyer_service_key_in_mode_label(keyer_key_in_mode_t mode);
const char *keyer_service_key_out_mode_label(keyer_key_out_mode_t mode);
const char *keyer_service_paddle_mode_label(keyer_paddle_mode_t mode);

void keyer_service_set_input_mode(keyer_input_mode_t mode);
uint16_t keyer_service_get_tx_wpm(void);
void keyer_service_set_tx_wpm(uint16_t wpm);
void keyer_service_adjust_tx_wpm(int delta);
uint16_t keyer_service_get_dit_ms(void);
void keyer_service_play_text(const char *text);
void keyer_service_stop_tx(void);
bool keyer_service_is_tx_active(void);
bool keyer_service_tx_append_text(const char *text, bool insert_space);
bool keyer_service_tx_backspace(void);
void keyer_service_tx_clear(void);
void keyer_service_tx_start(void);
bool keyer_service_tx_has_text(void);
void keyer_service_tx_copy_text(char *destination, size_t destination_size);
uint32_t keyer_service_tx_revision(void);
void keyer_service_set_tune_active(bool active);
bool keyer_service_get_tune_active(void);
void keyer_service_set_tune_latched(bool latched);
bool keyer_service_get_tune_latched(void);
bool keyer_service_get_tune_output_active(void);
void keyer_service_update(void);
keyer_event_t keyer_service_poll_event(void);

#ifdef __cplusplus
}
#endif

/*
 * keyer_service
 *
 * Responsibility: Owns paddle/key input abstraction for straight key, single
 * paddle, and dual paddle future support.
 * Hardware ownership: paddle/key GPIO or its HAL owner. Other modules consume
 * keyer events and must not read raw key/paddle GPIO.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KEYER_INPUT_STRAIGHT_KEY = 0,
    KEYER_INPUT_SINGLE_PADDLE,
    KEYER_INPUT_DUAL_PADDLE,
} keyer_input_mode_t;

typedef enum {
    KEYER_IO_PADDLE = 0,
    KEYER_IO_PADDLE_R,
    KEYER_IO_SK,
    KEYER_IO_SK_MONO,
} keyer_io_mode_t;

typedef enum {
    KEYER_EVENT_NONE = 0,
    KEYER_EVENT_DIT,
    KEYER_EVENT_DAH,
    KEYER_EVENT_CHAR_COMPLETE,
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
keyer_io_mode_t keyer_service_get_key_in_mode(void);
void keyer_service_set_key_in_mode(keyer_io_mode_t mode);
void keyer_service_cycle_key_in_mode(int direction);
keyer_io_mode_t keyer_service_get_key_out_mode(void);
void keyer_service_set_key_out_mode(keyer_io_mode_t mode);
void keyer_service_cycle_key_out_mode(int direction);
uint8_t keyer_service_get_key_in_wpm(void);
void keyer_service_set_key_in_wpm(uint8_t wpm);
void keyer_service_adjust_key_in_wpm(int delta);
uint8_t keyer_service_get_key_out_wpm(void);
void keyer_service_set_key_out_wpm(uint8_t wpm);
void keyer_service_adjust_key_out_wpm(int delta);
const char *keyer_service_io_mode_label(keyer_io_mode_t mode);

void keyer_service_set_input_mode(keyer_input_mode_t mode);
uint16_t keyer_service_get_tx_wpm(void);
void keyer_service_set_tx_wpm(uint16_t wpm);
void keyer_service_adjust_tx_wpm(int delta);
void keyer_service_update(void);
keyer_event_t keyer_service_poll_event(void);

#ifdef __cplusplus
}
#endif

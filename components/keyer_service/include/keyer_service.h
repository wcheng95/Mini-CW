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
void keyer_service_set_input_mode(keyer_input_mode_t mode);
void keyer_service_update(void);
keyer_event_t keyer_service_poll_event(void);

#ifdef __cplusplus
}
#endif

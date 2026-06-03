/*
 * keyer_decoder
 *
 * Private reusable Morse pattern decoder for keyer_service input paths.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KEYER_DECODER_MAX_ELEMENTS 12U

typedef enum {
    KEYER_DECODER_RESULT_NONE = 0,
    KEYER_DECODER_RESULT_CHAR,
    KEYER_DECODER_RESULT_BACKSPACE,
    KEYER_DECODER_RESULT_ENTER,
    KEYER_DECODER_RESULT_SPACE,
    KEYER_DECODER_RESULT_INVALID,
} keyer_decoder_result_type_t;

typedef struct {
    keyer_decoder_result_type_t type;
    char ch;
} keyer_decoder_result_t;

typedef struct {
    char pattern[KEYER_DECODER_MAX_ELEMENTS + 1U];
    uint8_t len;
    bool overflow;
} keyer_decoder_t;

void keyer_decoder_reset(keyer_decoder_t *decoder);
bool keyer_decoder_has_pending(const keyer_decoder_t *decoder);
void keyer_decoder_append(keyer_decoder_t *decoder, bool dah);
keyer_decoder_result_t keyer_decoder_finalize(keyer_decoder_t *decoder);

#ifdef __cplusplus
}
#endif

/*
 * keyer_decoder
 *
 * Converts completed Morse element patterns into Mini-CW input characters.
 */

#include "keyer_decoder.h"

#include <stddef.h>
#include <string.h>

typedef struct {
    const char *pattern;
    char ch;
} keyer_decoder_entry_t;

static const keyer_decoder_entry_t KEYER_DECODER_TABLE[] = {
    {".-", 'A'},     {"-...", 'B'},   {"-.-.", 'C'},   {"-..", 'D'},
    {".", 'E'},      {"..-.", 'F'},   {"--.", 'G'},    {"....", 'H'},
    {"..", 'I'},     {".---", 'J'},   {"-.-", 'K'},    {".-..", 'L'},
    {"--", 'M'},     {"-.", 'N'},     {"---", 'O'},    {".--.", 'P'},
    {"--.-", 'Q'},   {".-.", 'R'},    {"...", 'S'},    {"-", 'T'},
    {"..-", 'U'},    {"...-", 'V'},   {".--", 'W'},    {"-..-", 'X'},
    {"-.--", 'Y'},   {"--..", 'Z'},   {"-----", '0'},  {".----", '1'},
    {"..---", '2'},  {"...--", '3'},  {"....-", '4'},  {".....", '5'},
    {"-....", '6'},  {"--...", '7'},  {"---..", '8'},  {"----.", '9'},
    {".-.-.-", '.'}, {"--..--", ','}, {"..--..", '?'}, {"-..-.", '/'},
    {"-...-", '='},
};

static const keyer_decoder_result_t KEYER_DECODER_NONE = {
    .type = KEYER_DECODER_RESULT_NONE,
    .ch = '\0',
};

void keyer_decoder_reset(keyer_decoder_t *decoder)
{
    if (decoder == NULL) {
        return;
    }

    decoder->pattern[0] = '\0';
    decoder->len = 0U;
    decoder->overflow = false;
}

bool keyer_decoder_has_pending(const keyer_decoder_t *decoder)
{
    return decoder != NULL && (decoder->len > 0U || decoder->overflow);
}

void keyer_decoder_append(keyer_decoder_t *decoder, bool dah)
{
    if (decoder == NULL) {
        return;
    }

    if (decoder->len >= KEYER_DECODER_MAX_ELEMENTS) {
        decoder->overflow = true;
        return;
    }

    decoder->pattern[decoder->len++] = dah ? '-' : '.';
    decoder->pattern[decoder->len] = '\0';
}

keyer_decoder_result_t keyer_decoder_finalize(keyer_decoder_t *decoder)
{
    keyer_decoder_result_t result = KEYER_DECODER_NONE;

    if (decoder == NULL || !keyer_decoder_has_pending(decoder)) {
        return result;
    }

    if (decoder->overflow) {
        result.type = KEYER_DECODER_RESULT_INVALID;
        keyer_decoder_reset(decoder);
        return result;
    }

    if (strcmp(decoder->pattern, "........") == 0) {
        result.type = KEYER_DECODER_RESULT_BACKSPACE;
        result.ch = '\b';
        keyer_decoder_reset(decoder);
        return result;
    }

    if (strcmp(decoder->pattern, "----") == 0) {
        result.type = KEYER_DECODER_RESULT_SPACE;
        result.ch = ' ';
        keyer_decoder_reset(decoder);
        return result;
    }

    for (size_t i = 0U; i < sizeof(KEYER_DECODER_TABLE) / sizeof(KEYER_DECODER_TABLE[0]); ++i) {
        if (strcmp(decoder->pattern, KEYER_DECODER_TABLE[i].pattern) == 0) {
            result.type = KEYER_DECODER_RESULT_CHAR;
            result.ch = KEYER_DECODER_TABLE[i].ch;
            keyer_decoder_reset(decoder);
            return result;
        }
    }

    result.type = KEYER_DECODER_RESULT_INVALID;
    keyer_decoder_reset(decoder);
    return result;
}

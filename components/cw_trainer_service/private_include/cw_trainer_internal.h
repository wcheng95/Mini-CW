/*
 * Private helpers shared by cw_trainer_service mode implementations.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const char CW_TRAINER_KOCH_CHARS[];

uint8_t cw_trainer_clamp_u8(uint8_t value, uint8_t min_value, uint8_t max_value);
uint32_t cw_trainer_rand_u32(void);
void cw_trainer_seed_random(void);
uint16_t cw_trainer_levenshtein(const char *a, const char *b, uint16_t max_len);

#ifdef __cplusplus
}
#endif

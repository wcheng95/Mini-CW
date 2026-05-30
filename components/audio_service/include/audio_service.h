/*
 * audio_service
 *
 * Responsibility: Owns all speaker and CW tone output.
 * Hardware ownership: speaker/tone output. Other modules must request audio
 * through audio_service APIs and must not drive speaker hardware directly.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void audio_service_init(void);
void audio_service_set_volume(uint8_t percent);
uint16_t audio_service_get_tone_hz(void);
uint8_t audio_service_get_volume(void);
void audio_service_set_tone_hz(uint16_t tone_hz);
void audio_service_adjust_tone_hz(int delta_hz);
void audio_service_adjust_volume(int delta);
void audio_service_play_feedback_beep(void);
void audio_service_play_feedback_tone(void);
void audio_service_tone_on(void);
void audio_service_tone_off(void);
void audio_service_play_dit(uint16_t dit_ms);
void audio_service_play_dah(uint16_t dit_ms);
void audio_service_stop_all(void);
bool audio_service_is_busy(void);
void audio_service_set_cw_wpm(uint8_t wpm);
uint8_t audio_service_get_cw_wpm(void);
void audio_service_set_cw_farnsworth_wpm(uint8_t effective_wpm);
uint8_t audio_service_get_cw_farnsworth_wpm(void);
const char *audio_service_get_cw_pattern(char ch);
void audio_service_play_cw_text(const char *text);
void audio_service_play_cw_char(char ch);
void audio_service_play_cw_pattern(const char *pattern);
void audio_service_play_cw_symbol(char symbol);

#ifdef __cplusplus
}
#endif

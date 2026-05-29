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
void audio_service_play_feedback_tone(void);
void audio_cw_set_pitch(uint16_t hz);
void audio_cw_set_wpm(uint8_t wpm);
void audio_cw_set_farnsworth_wpm(uint8_t effective_wpm);
uint16_t audio_cw_get_pitch(void);
uint8_t audio_cw_get_wpm(void);
uint8_t audio_cw_get_farnsworth_wpm(void);
const char *audio_cw_get_pattern(char ch);
void audio_cw_play_text(const char *text);
void audio_cw_play_char(char ch);
void audio_cw_play_pattern(const char *pattern);
void audio_cw_play_symbol(char symbol);
void audio_cw_start_sidetone(void);
void audio_cw_stop_sidetone(void);
void audio_cw_stop(void);
bool audio_cw_is_busy(void);

#ifdef __cplusplus
}
#endif

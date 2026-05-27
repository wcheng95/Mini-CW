/*
 * audio_output_port
 *
 * Responsibility: Private speaker hardware port used only by audio_service.
 * Hardware ownership: Cardputer ADV speaker path. This is where ES8311/I2S
 * driver code belongs; no other module should touch codec, I2S, or speaker
 * output APIs directly.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int sample_rate_hz;
    int channels;
    int bits_per_sample;
    uint8_t volume_percent;
} audio_output_port_config_t;

bool audio_output_port_init(const audio_output_port_config_t *config);
void audio_output_port_set_volume(uint8_t percent);
bool audio_output_port_write_pcm(const int16_t *samples,
                                 size_t sample_count,
                                 size_t *bytes_written);
bool audio_output_port_is_initialized(void);

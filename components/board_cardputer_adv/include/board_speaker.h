#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int sample_rate;        // Example: 48000
    int channels;           // For now use 1
    int bits_per_sample;    // For now use 16
    int volume;             // 0-100
} board_speaker_config_t;

esp_err_t board_speaker_init(const board_speaker_config_t* cfg);

esp_err_t board_speaker_write(const int16_t* samples,
                              size_t sample_count,
                              size_t* bytes_written);

void board_speaker_deinit(void);

bool board_speaker_is_initialized(void);

#ifdef __cplusplus
}
#endif
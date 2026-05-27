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
    float gain_db;          // Example: 20.0f or 30.0f
} board_mic_config_t;

esp_err_t board_mic_init(const board_mic_config_t* cfg);

// Returns ESP_OK on success.
// bytes_read can be nullptr if caller does not need it.
esp_err_t board_mic_read(int16_t* samples, size_t sample_count, size_t* bytes_read);

void board_mic_deinit(void);

bool board_mic_is_initialized(void);

#ifdef __cplusplus
}
#endif
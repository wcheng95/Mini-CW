/*
 * audio_output_port
 *
 * Responsibility: Private speaker hardware port used only by audio_service.
 * Hardware ownership: Cardputer ADV speaker path. This wrapper delegates to
 * the known-good mic_test board_audio backend for ES8311/I2S output.
 */

#include "audio_output_port.h"

#include <stdbool.h>

#include "board_audio.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "audio_output_port";

static audio_output_port_config_t s_config = {
    .sample_rate_hz = 48000,
    .channels = 1,
    .bits_per_sample = 16,
    .volume_percent = 80,
};

static bool s_initialized;

static uint8_t clamp_percent(uint8_t percent)
{
    return percent > 100 ? 100 : percent;
}

bool audio_output_port_init(const audio_output_port_config_t *config)
{
    if (s_initialized || board_audio_is_initialized()) {
        s_initialized = true;
        return true;
    }

    if (config != NULL) {
        s_config = *config;
    }

    if (s_config.sample_rate_hz <= 0) {
        s_config.sample_rate_hz = 48000;
    }

    if (s_config.channels <= 0) {
        s_config.channels = 1;
    }

    if (s_config.bits_per_sample <= 0) {
        s_config.bits_per_sample = 16;
    }

    s_config.volume_percent = clamp_percent(s_config.volume_percent);

    board_audio_config_t board_cfg = {
        .sample_rate = s_config.sample_rate_hz,
        .channels = s_config.channels,
        .bits_per_sample = s_config.bits_per_sample,
        .mic_gain_db = 30.0f,
        .speaker_volume = s_config.volume_percent,
    };

    esp_err_t err = board_audio_init(&board_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "board_audio_init failed: %s", esp_err_to_name(err));
        ESP_LOGW(TAG, "audio_service will log tone timing without hardware output");
        s_initialized = false;
        return false;
    }

    s_initialized = true;
    ESP_LOGI(TAG,
             "board_audio speaker backend ready: rate=%d channels=%d bits=%d volume=%u",
             s_config.sample_rate_hz,
             s_config.channels,
             s_config.bits_per_sample,
             (unsigned)s_config.volume_percent);
    return true;
}

void audio_output_port_set_volume(uint8_t percent)
{
    s_config.volume_percent = clamp_percent(percent);

    if (!s_initialized || !board_audio_is_initialized()) {
        ESP_LOGI(TAG,
                 "set output volume requested: %u (applied on next board_audio init)",
                 (unsigned)s_config.volume_percent);
        return;
    }

    esp_err_t err = board_audio_set_speaker_volume(s_config.volume_percent);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "set output volume failed: %s",
                 esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "set output volume: %u", (unsigned)s_config.volume_percent);
}

bool audio_output_port_write_pcm(const int16_t *samples,
                                 size_t sample_count,
                                 size_t *bytes_written)
{
    if (bytes_written != NULL) {
        *bytes_written = 0;
    }

    if (!s_initialized || samples == NULL || sample_count == 0) {
        return false;
    }

    esp_err_t err = board_audio_write(samples, sample_count, bytes_written);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "board_audio_write failed: %s", esp_err_to_name(err));
        return false;
    }

    return true;
}

bool audio_output_port_is_initialized(void)
{
    return s_initialized && board_audio_is_initialized();
}

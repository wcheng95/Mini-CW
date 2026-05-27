/*
 * cw_trainer_service
 *
 * Responsibility: Owns high-level CW trainer session state for RX and TX
 * practice.
 * Hardware ownership: none. Milestone 2 handles high-level character input and
 * requests CW playback through audio_service; it never touches speaker hardware.
 */

#include "cw_trainer_service.h"

#include "audio_service.h"
#include "esp_log.h"
#include <stdbool.h>
#include <ctype.h>
#include <stdio.h>

static const char *TAG = "cw_trainer_service";

static char s_target_text[64];
static char s_copy_text[64];
static const char *s_last_pattern;
static char s_last_char;
static const char *s_status = "Ready";
static bool s_rx_active;
static bool s_tx_active;
static bool s_tone_test_active;

void cw_trainer_service_init(void)
{
    snprintf(s_target_text, sizeof(s_target_text), "%s", "CQ MINI CW");
    s_copy_text[0] = '\0';
    s_last_pattern = "";
    s_last_char = '\0';
    s_rx_active = false;
    s_tx_active = false;
    s_tone_test_active = false;

    ESP_LOGI(TAG, "initialized trainer service");
}

void cw_trainer_start_rx_practice(void)
{
    s_rx_active = true;
    s_tx_active = false;
    s_tone_test_active = false;
    s_copy_text[0] = '\0';

    ESP_LOGI(TAG, "start RX practice stub; target='%s'", s_target_text);
}

void cw_trainer_start_tx_practice(void)
{
    s_rx_active = false;
    s_tx_active = true;
    s_tone_test_active = false;
    s_copy_text[0] = '\0';

    ESP_LOGI(TAG, "start TX practice stub; target='%s'", s_target_text);
}

void cw_trainer_start_tone_test(void)
{
    s_rx_active = false;
    s_tx_active = false;
    s_tone_test_active = true;
    s_status = "Ready";

    ESP_LOGI(TAG, "start tone test");
}

void cw_trainer_stop(void)
{
    if (s_rx_active || s_tx_active || s_tone_test_active) {
        ESP_LOGI(TAG, "stop practice");
    }

    s_rx_active = false;
    s_tx_active = false;
    s_tone_test_active = false;
    s_status = "Stopped";
}

void cw_trainer_handle_keyer_event(const keyer_event_t *event)
{
    if (event == NULL || event->type == KEYER_EVENT_NONE) {
        return;
    }

    ESP_LOGI(TAG, "keyer event: type=%d char='%c' duration=%u ms",
             event->type,
             event->decoded_char ? event->decoded_char : ' ',
             (unsigned)event->duration_ms);
}

bool cw_trainer_handle_char_input(char ch)
{
    char normalized = (char)toupper((unsigned char)ch);
    const char *pattern = audio_cw_get_pattern(normalized);

    if (pattern == NULL) {
        s_status = "Unsupported";
        ESP_LOGW(TAG, "unsupported character input: '%c'", ch);
        return false;
    }

    s_last_char = normalized;
    s_last_pattern = pattern;
    s_status = "Queued";

    ESP_LOGI(TAG, "character input: %c %s", s_last_char, s_last_pattern);
    audio_cw_play_char(normalized);
    return true;
}

void cw_trainer_adjust_wpm(int delta)
{
    int next = (int)audio_cw_get_wpm() + delta;
    if (next < 0) {
        next = 0;
    }

    audio_cw_set_wpm((uint8_t)next);
    s_status = "WPM updated";
}

void cw_trainer_adjust_pitch(int delta_hz)
{
    int next = (int)audio_cw_get_pitch() + delta_hz;
    if (next < 0) {
        next = 0;
    }

    audio_cw_set_pitch((uint16_t)next);
    s_status = "Pitch updated";
}

const char *cw_trainer_get_target_text(void)
{
    return s_target_text;
}

const char *cw_trainer_get_copy_text(void)
{
    return s_copy_text;
}

char cw_trainer_get_last_char(void)
{
    return s_last_char;
}

const char *cw_trainer_get_last_pattern(void)
{
    return s_last_pattern;
}

const char *cw_trainer_get_status(void)
{
    return s_status;
}

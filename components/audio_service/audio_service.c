/*
 * audio_service
 *
 * Responsibility: Owns all speaker and CW tone output.
 * Hardware ownership: speaker/tone output. Milestone 2 converts characters to
 * Morse timing and writes PCM through the private audio output port.
 */

#include "audio_service.h"

#include "audio_output_port.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const char *TAG = "audio_service";

#define AUDIO_CW_MIN_WPM 5
#define AUDIO_CW_MAX_WPM 40
#define AUDIO_CW_MIN_PITCH_HZ 300
#define AUDIO_CW_MAX_PITCH_HZ 1000
#define AUDIO_CW_MAX_SAMPLE_RATE_HZ 48000U
#define AUDIO_CW_DEFAULT_SAMPLE_RATE_HZ 48000U
#define AUDIO_CW_TONE_CHUNK_MS 10
#define AUDIO_CW_ENVELOPE_MS 3
#define AUDIO_CW_STOP_SILENCE_MS 80
#define AUDIO_CW_AMPLITUDE 12000
#define AUDIO_CW_TASK_STACK_BYTES 6144
#define AUDIO_CW_COMMAND_TEXT_MAX 64
#define AUDIO_CW_COMMAND_PATTERN_MAX 16
#define AUDIO_SERVICE_DEFAULT_VOLUME_PERCENT 20
#define AUDIO_SERVICE_FEEDBACK_TONE_MS 50
#define AUDIO_CW_TWO_PI 6.28318530717958647692f

typedef struct {
    char ch;
    const char *pattern;
} morse_entry_t;

typedef enum {
    AUDIO_CW_COMMAND_NONE = 0,
    AUDIO_CW_COMMAND_TEXT,
    AUDIO_CW_COMMAND_CHAR,
    AUDIO_CW_COMMAND_PATTERN,
    AUDIO_CW_COMMAND_FEEDBACK_TONE,
} audio_cw_command_type_t;

typedef struct {
    audio_cw_command_type_t type;
    char text[AUDIO_CW_COMMAND_TEXT_MAX];
    char ch;
    char pattern[AUDIO_CW_COMMAND_PATTERN_MAX];
} audio_cw_command_t;

static const morse_entry_t MORSE_TABLE[] = {
    {'A', ".-"},
    {'B', "-..."},
    {'C', "-.-."},
    {'D', "-.."},
    {'E', "."},
    {'F', "..-."},
    {'G', "--."},
    {'H', "...."},
    {'I', ".."},
    {'J', ".---"},
    {'K', "-.-"},
    {'L', ".-.."},
    {'M', "--"},
    {'N', "-."},
    {'O', "---"},
    {'P', ".--."},
    {'Q', "--.-"},
    {'R', ".-."},
    {'S', "..."},
    {'T', "-"},
    {'U', "..-"},
    {'V', "...-"},
    {'W', ".--"},
    {'X', "-..-"},
    {'Y', "-.--"},
    {'Z', "--.."},
    {'0', "-----"},
    {'1', ".----"},
    {'2', "..---"},
    {'3', "...--"},
    {'4', "....-"},
    {'5', "....."},
    {'6', "-...."},
    {'7', "--..."},
    {'8', "---.."},
    {'9', "----."},
};

static uint16_t s_pitch_hz = 700;
static uint8_t s_wpm = 20;
static uint8_t s_farnsworth_wpm = 12;
static uint8_t s_volume_percent = AUDIO_SERVICE_DEFAULT_VOLUME_PERCENT;
static bool s_output_ready;
static volatile bool s_busy;
static volatile bool s_stop_requested;
static volatile bool s_command_pending;
static TaskHandle_t s_audio_task;
static SemaphoreHandle_t s_command_mutex;
static audio_cw_command_t s_pending_command;

static audio_output_port_config_t s_output_config = {
    .sample_rate_hz = AUDIO_CW_DEFAULT_SAMPLE_RATE_HZ,
    .channels = 1,
    .bits_per_sample = 16,
    .volume_percent = AUDIO_SERVICE_DEFAULT_VOLUME_PERCENT,
};

static uint8_t clamp_percent(uint8_t percent)
{
    return percent > 100 ? 100 : percent;
}

static uint8_t clamp_wpm(uint8_t wpm)
{
    if (wpm < AUDIO_CW_MIN_WPM) {
        return AUDIO_CW_MIN_WPM;
    }

    if (wpm > AUDIO_CW_MAX_WPM) {
        return AUDIO_CW_MAX_WPM;
    }

    return wpm;
}

static uint16_t clamp_pitch(uint16_t hz)
{
    if (hz < AUDIO_CW_MIN_PITCH_HZ) {
        return AUDIO_CW_MIN_PITCH_HZ;
    }

    if (hz > AUDIO_CW_MAX_PITCH_HZ) {
        return AUDIO_CW_MAX_PITCH_HZ;
    }

    return hz;
}

static uint32_t dit_ms(void)
{
    return 1200U / s_wpm;
}

static bool audio_cw_lock(TickType_t timeout)
{
    if (s_command_mutex == NULL) {
        return true;
    }

    return xSemaphoreTake(s_command_mutex, timeout) == pdTRUE;
}

static void audio_cw_unlock(void)
{
    if (s_command_mutex != NULL) {
        xSemaphoreGive(s_command_mutex);
    }
}

static bool audio_cw_sample_rate_supported(int sample_rate_hz)
{
    /*
     * The CW tone generator currently supports output rates up to 48 kHz. Its
     * local stack PCM buffers are sized from AUDIO_CW_MAX_SAMPLE_RATE_HZ, so a
     * higher runtime rate is rejected before chunk generation.
     */
    return sample_rate_hz > 0 &&
           sample_rate_hz <= (int)AUDIO_CW_MAX_SAMPLE_RATE_HZ;
}

static void audio_cw_write_silence_ms(uint32_t duration_ms, bool interruptible)
{
    if (!audio_cw_sample_rate_supported(s_output_config.sample_rate_hz)) {
        return;
    }

    const uint32_t sample_rate = (uint32_t)s_output_config.sample_rate_hz;
    const uint32_t total_samples = (uint32_t)(((uint64_t)sample_rate * duration_ms) / 1000U);
    const uint32_t max_chunk_samples =
        (uint32_t)(((uint64_t)sample_rate * AUDIO_CW_TONE_CHUNK_MS) / 1000U);
    int16_t samples[(AUDIO_CW_MAX_SAMPLE_RATE_HZ * AUDIO_CW_TONE_CHUNK_MS) / 1000U];
    uint32_t samples_written = 0;

    memset(samples, 0, sizeof(samples));

    if (max_chunk_samples == 0) {
        vTaskDelay(pdMS_TO_TICKS(duration_ms));
        return;
    }

    while (samples_written < total_samples) {
        if (interruptible && s_stop_requested) {
            break;
        }

        uint32_t remaining = total_samples - samples_written;
        uint32_t chunk_samples = remaining < max_chunk_samples ? remaining : max_chunk_samples;

        size_t bytes_written = 0;
        bool wrote = s_output_ready &&
                     audio_output_port_write_pcm(samples, chunk_samples, &bytes_written);
        if (!wrote) {
            vTaskDelay(pdMS_TO_TICKS((chunk_samples * 1000U) / sample_rate));
        }

        samples_written += chunk_samples;
    }
}

static void audio_cw_calculate_envelope_samples(uint32_t total_samples,
                                                uint32_t sample_rate,
                                                uint32_t *attack_samples,
                                                uint32_t *release_samples)
{
    /*
     * The click-reduction envelope is defined in milliseconds, then converted
     * to samples using the active output sample rate. At 48 kHz, 3 ms is 144
     * samples. Very short tones cannot fit a full attack plus release, so the
     * envelope is scaled to the tone length instead of exceeding it.
     */
    uint32_t nominal_samples =
        (uint32_t)(((uint64_t)sample_rate * AUDIO_CW_ENVELOPE_MS) / 1000U);

    if (nominal_samples == 0 && total_samples > 0) {
        nominal_samples = 1;
    }

    *attack_samples = nominal_samples;
    *release_samples = nominal_samples;

    if (*attack_samples + *release_samples > total_samples) {
        *attack_samples = total_samples / 2U;
        *release_samples = total_samples - *attack_samples;
    }
}

static float audio_cw_envelope_gain(uint32_t sample_index,
                                    uint32_t total_samples,
                                    uint32_t attack_samples,
                                    uint32_t release_samples)
{
    float gain = 1.0f;

    if (total_samples == 0) {
        return 0.0f;
    }

    if (attack_samples > 0 && sample_index < attack_samples) {
        gain = attack_samples > 1
                   ? (float)sample_index / (float)(attack_samples - 1U)
                   : 0.0f;
    }

    if (release_samples > 0) {
        const uint32_t release_start = total_samples - release_samples;
        if (sample_index >= release_start) {
            float release_gain = release_samples > 1
                                     ? (float)(total_samples - 1U - sample_index) /
                                           (float)(release_samples - 1U)
                                     : 0.0f;
            if (release_gain < gain) {
                gain = release_gain;
            }
        }
    }

    return gain;
}

static void audio_cw_gap_units(uint32_t units)
{
    uint32_t delay_ms = dit_ms() * units;
    if (delay_ms > 0) {
        audio_cw_write_silence_ms(delay_ms, true);
    }
}

static void audio_cw_write_tone_ms(uint32_t duration_ms)
{
    if (!audio_cw_sample_rate_supported(s_output_config.sample_rate_hz)) {
        return;
    }

    const uint32_t sample_rate = (uint32_t)s_output_config.sample_rate_hz;
    const uint32_t total_samples = (uint32_t)(((uint64_t)sample_rate * duration_ms) / 1000U);
    const uint32_t max_chunk_samples =
        (uint32_t)(((uint64_t)sample_rate * AUDIO_CW_TONE_CHUNK_MS) / 1000U);
    int16_t samples[(AUDIO_CW_MAX_SAMPLE_RATE_HZ * AUDIO_CW_TONE_CHUNK_MS) / 1000U];
    uint32_t samples_written = 0;
    uint32_t attack_samples = 0;
    uint32_t release_samples = 0;
    float phase = 0.0f;

    if (total_samples == 0 || max_chunk_samples == 0) {
        return;
    }

    const float phase_step = (AUDIO_CW_TWO_PI * (float)s_pitch_hz) / (float)sample_rate;

    audio_cw_calculate_envelope_samples(total_samples,
                                        sample_rate,
                                        &attack_samples,
                                        &release_samples);

    /*
     * A sine sidetone is less harsh than the previous square wave because it
     * avoids strong odd harmonics. The 3 ms attack/release envelope prevents
     * sharp jumps between silence and tone, which is the main source of clicks.
     *
     * Phase is local to one dit/dah and intentionally preserved across all
     * chunks written by this call, so chunk boundaries cannot create phase
     * discontinuities. Separate Morse elements ramp down to silence and then
     * ramp up again, so global phase continuity between calls is unnecessary.
     */
    while (samples_written < total_samples && !s_stop_requested) {
        uint32_t remaining = total_samples - samples_written;
        uint32_t chunk_samples = remaining < max_chunk_samples ? remaining : max_chunk_samples;

        for (uint32_t i = 0; i < chunk_samples; ++i) {
            uint32_t sample_index = samples_written + i;
            float envelope = audio_cw_envelope_gain(sample_index,
                                                    total_samples,
                                                    attack_samples,
                                                    release_samples);
            float sample = sinf(phase) * (float)AUDIO_CW_AMPLITUDE * envelope;

            samples[i] = (int16_t)sample;

            phase += phase_step;
            if (phase >= AUDIO_CW_TWO_PI) {
                phase -= AUDIO_CW_TWO_PI;
            }
        }

        size_t bytes_written = 0;
        bool wrote = s_output_ready &&
                     audio_output_port_write_pcm(samples, chunk_samples, &bytes_written);
        if (!wrote) {
            vTaskDelay(pdMS_TO_TICKS((chunk_samples * 1000U) / sample_rate));
        }

        samples_written += chunk_samples;
    }
}

static void audio_cw_play_pattern_blocking(const char *pattern)
{
    if (pattern == NULL || pattern[0] == '\0') {
        return;
    }

    uint32_t unit_ms = dit_ms();
    for (const char *p = pattern; *p != '\0' && !s_stop_requested; ++p) {
        uint32_t units = 0;
        if (*p == '.') {
            units = 1;
        } else if (*p == '-') {
            units = 3;
        } else {
            ESP_LOGW(TAG, "unsupported Morse element: '%c'", *p);
            continue;
        }

        ESP_LOGI(TAG, "tone: %c %u ms", *p, (unsigned)(unit_ms * units));
        audio_cw_write_tone_ms(unit_ms * units);

        if (p[1] != '\0' && !s_stop_requested) {
            ESP_LOGI(TAG, "element gap: %u ms", (unsigned)unit_ms);
            audio_cw_gap_units(1);
        }
    }
}

static void audio_cw_play_char_blocking(char ch)
{
    char normalized = (char)toupper((unsigned char)ch);
    const char *pattern = audio_cw_get_pattern(normalized);
    if (pattern == NULL) {
        ESP_LOGW(TAG, "unsupported character: '%c'", ch);
        return;
    }

    ESP_LOGI(TAG, "play char: %c %s dit=%u ms pitch=%u Hz",
             normalized,
             pattern,
             (unsigned)dit_ms(),
             (unsigned)s_pitch_hz);
    audio_cw_play_pattern_blocking(pattern);

    if (!s_stop_requested) {
        ESP_LOGI(TAG, "character gap: %u ms", (unsigned)(dit_ms() * 3U));
        audio_cw_gap_units(3);
    }
}

static void audio_cw_play_text_blocking(const char *text)
{
    if (text == NULL) {
        return;
    }

    ESP_LOGI(TAG, "play text: %s", text);

    for (const char *p = text; *p != '\0' && !s_stop_requested; ++p) {
        if (*p == ' ') {
            ESP_LOGI(TAG, "word gap: %u ms", (unsigned)(dit_ms() * 7U));
            audio_cw_gap_units(7);
            continue;
        }

        const char *pattern = audio_cw_get_pattern(*p);
        if (pattern == NULL) {
            ESP_LOGW(TAG, "unsupported text character: '%c'", *p);
            continue;
        }

        ESP_LOGI(TAG, "text char: %c %s", (char)toupper((unsigned char)*p), pattern);
        audio_cw_play_pattern_blocking(pattern);

        if (p[1] != '\0' && p[1] != ' ' && !s_stop_requested) {
            ESP_LOGI(TAG, "character gap: %u ms", (unsigned)(dit_ms() * 3U));
            audio_cw_gap_units(3);
        }
    }
}

static void audio_cw_run_command(const audio_cw_command_t *command)
{
    if (command == NULL) {
        return;
    }

    switch (command->type) {
    case AUDIO_CW_COMMAND_TEXT:
        audio_cw_play_text_blocking(command->text);
        break;
    case AUDIO_CW_COMMAND_CHAR:
        audio_cw_play_char_blocking(command->ch);
        break;
    case AUDIO_CW_COMMAND_PATTERN:
        audio_cw_play_pattern_blocking(command->pattern);
        break;
    case AUDIO_CW_COMMAND_FEEDBACK_TONE:
        audio_cw_write_tone_ms(AUDIO_SERVICE_FEEDBACK_TONE_MS);
        break;
    case AUDIO_CW_COMMAND_NONE:
    default:
        break;
    }
}

static bool audio_cw_take_pending_command(audio_cw_command_t *out_command)
{
    bool has_command = false;

    if (out_command == NULL) {
        return false;
    }

    if (audio_cw_lock(portMAX_DELAY)) {
        if (s_command_pending) {
            *out_command = s_pending_command;
            memset(&s_pending_command, 0, sizeof(s_pending_command));
            s_command_pending = false;
            has_command = true;
        }

        audio_cw_unlock();
    }

    return has_command;
}

static void audio_cw_clear_pending_command(void)
{
    if (audio_cw_lock(pdMS_TO_TICKS(20))) {
        memset(&s_pending_command, 0, sizeof(s_pending_command));
        s_command_pending = false;
        audio_cw_unlock();
    }
}

static void audio_cw_task(void *arg)
{
    (void)arg;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        for (;;) {
            audio_cw_command_t command;
            if (!audio_cw_take_pending_command(&command)) {
                break;
            }

            s_stop_requested = false;
            s_busy = true;
            audio_cw_run_command(&command);
            audio_cw_write_silence_ms(AUDIO_CW_STOP_SILENCE_MS, false);
            s_busy = false;
        }
    }
}

static void audio_cw_queue_command(const audio_cw_command_t *command)
{
    if (command == NULL) {
        return;
    }

    s_stop_requested = true;

    if (audio_cw_lock(pdMS_TO_TICKS(20))) {
        s_pending_command = *command;
        s_command_pending = true;
        audio_cw_unlock();
    } else {
        ESP_LOGW(TAG, "audio command queue busy; dropping command");
        return;
    }

    if (s_audio_task != NULL) {
        xTaskNotifyGive(s_audio_task);
        return;
    }

    ESP_LOGW(TAG, "audio task unavailable; running command synchronously");
    audio_cw_clear_pending_command();
    s_stop_requested = false;
    s_busy = true;
    audio_cw_run_command(command);
    audio_cw_write_silence_ms(AUDIO_CW_STOP_SILENCE_MS, false);
    s_busy = false;
}

void audio_service_init(void)
{
    s_output_config.volume_percent = s_volume_percent;
    s_wpm = clamp_wpm(s_wpm);
    s_pitch_hz = clamp_pitch(s_pitch_hz);
    s_output_ready = audio_output_port_init(&s_output_config);

    if (s_command_mutex == NULL) {
        s_command_mutex = xSemaphoreCreateMutex();
        if (s_command_mutex == NULL) {
            ESP_LOGW(TAG, "failed to create audio command mutex");
        }
    }

    if (s_audio_task == NULL) {
        BaseType_t task_ok = xTaskCreate(audio_cw_task,
                                         "audio_cw",
                                         AUDIO_CW_TASK_STACK_BYTES,
                                         NULL,
                                         5,
                                         &s_audio_task);
        if (task_ok != pdPASS) {
            s_audio_task = NULL;
            ESP_LOGW(TAG, "failed to create audio CW task");
        }
    }

    ESP_LOGI(TAG, "initialized speaker/tone owner");
    ESP_LOGI(TAG, "defaults: pitch=%u Hz wpm=%u farnsworth=%u volume=%u",
             (unsigned)s_pitch_hz,
             (unsigned)s_wpm,
             (unsigned)s_farnsworth_wpm,
             (unsigned)s_volume_percent);
    ESP_LOGI(TAG, "tone output: %s", s_output_ready ? "ES8311/I2S" : "log/timing fallback");
    ESP_LOGI(TAG, "CW playback: one-slot audio task, dit=1200/WPM");
}

void audio_service_set_volume(uint8_t percent)
{
    s_volume_percent = clamp_percent(percent);
    s_output_config.volume_percent = s_volume_percent;
    audio_output_port_set_volume(s_volume_percent);
    ESP_LOGI(TAG, "set volume: %u", (unsigned)s_volume_percent);
}

uint16_t audio_service_get_tone_hz(void)
{
    /*
     * Tone frequency is owned by audio_service. UI reads this for display only;
     * it does not duplicate or modify audio state.
     */
    return s_pitch_hz;
}

uint8_t audio_service_get_volume(void)
{
    /*
     * Speaker volume is owned by audio_service and applied through the private
     * audio output port.
     */
    return s_volume_percent;
}

void audio_service_set_tone_hz(uint16_t tone_hz)
{
    s_pitch_hz = clamp_pitch(tone_hz);
    ESP_LOGI(TAG, "set tone: %u Hz", (unsigned)s_pitch_hz);
}

void audio_service_adjust_tone_hz(int delta_hz)
{
    int next = (int)s_pitch_hz + delta_hz;
    if (next < AUDIO_CW_MIN_PITCH_HZ) {
        next = AUDIO_CW_MIN_PITCH_HZ;
    }

    if (next > AUDIO_CW_MAX_PITCH_HZ) {
        next = AUDIO_CW_MAX_PITCH_HZ;
    }

    audio_service_set_tone_hz((uint16_t)next);
}

void audio_service_adjust_volume(int delta)
{
    int next = (int)s_volume_percent + delta;
    if (next < 0) {
        next = 0;
    }

    if (next > 100) {
        next = 100;
    }

    audio_service_set_volume((uint8_t)next);
}

void audio_service_play_feedback_tone(void)
{
    audio_cw_command_t command = {
        .type = AUDIO_CW_COMMAND_FEEDBACK_TONE,
    };

    ESP_LOGI(TAG, "queue feedback tone: %u ms", (unsigned)AUDIO_SERVICE_FEEDBACK_TONE_MS);
    audio_cw_queue_command(&command);
}

void audio_cw_set_pitch(uint16_t hz)
{
    audio_service_set_tone_hz(hz);
}

void audio_cw_set_wpm(uint8_t wpm)
{
    s_wpm = clamp_wpm(wpm);
    ESP_LOGI(TAG, "set wpm: %u", (unsigned)s_wpm);
}

void audio_cw_set_farnsworth_wpm(uint8_t effective_wpm)
{
    s_farnsworth_wpm = effective_wpm;
    ESP_LOGI(TAG, "set farnsworth wpm: %u", (unsigned)s_farnsworth_wpm);
}

uint16_t audio_cw_get_pitch(void)
{
    return audio_service_get_tone_hz();
}

uint8_t audio_cw_get_wpm(void)
{
    return s_wpm;
}

uint8_t audio_cw_get_farnsworth_wpm(void)
{
    return s_farnsworth_wpm;
}

const char *audio_cw_get_pattern(char ch)
{
    char normalized = (char)toupper((unsigned char)ch);
    for (size_t i = 0; i < sizeof(MORSE_TABLE) / sizeof(MORSE_TABLE[0]); ++i) {
        if (MORSE_TABLE[i].ch == normalized) {
            return MORSE_TABLE[i].pattern;
        }
    }

    return NULL;
}

void audio_cw_play_text(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }

    audio_cw_command_t command = {
        .type = AUDIO_CW_COMMAND_TEXT,
    };
    strncpy(command.text, text, sizeof(command.text) - 1U);
    command.text[sizeof(command.text) - 1U] = '\0';

    ESP_LOGI(TAG, "queue text: %s", command.text);
    audio_cw_queue_command(&command);
}

void audio_cw_play_char(char ch)
{
    char normalized = (char)toupper((unsigned char)ch);
    const char *pattern = audio_cw_get_pattern(normalized);
    if (pattern == NULL) {
        ESP_LOGW(TAG, "unsupported character: '%c'", ch);
        return;
    }

    audio_cw_command_t command = {
        .type = AUDIO_CW_COMMAND_CHAR,
        .ch = normalized,
    };

    ESP_LOGI(TAG, "queue char: %c %s", normalized, pattern);
    audio_cw_queue_command(&command);
}

void audio_cw_play_pattern(const char *pattern)
{
    if (pattern == NULL || pattern[0] == '\0') {
        return;
    }

    audio_cw_command_t command = {
        .type = AUDIO_CW_COMMAND_PATTERN,
    };
    strncpy(command.pattern, pattern, sizeof(command.pattern) - 1U);
    command.pattern[sizeof(command.pattern) - 1U] = '\0';

    ESP_LOGI(TAG, "queue pattern: %s", command.pattern);
    audio_cw_queue_command(&command);
}

void audio_cw_play_symbol(char symbol)
{
    audio_cw_play_char(symbol);
}

void audio_cw_start_sidetone(void)
{
    ESP_LOGI(TAG, "start sidetone TODO: non-blocking sidetone task");
}

void audio_cw_stop_sidetone(void)
{
    audio_cw_stop();
    ESP_LOGI(TAG, "stop sidetone");
}

void audio_cw_stop(void)
{
    s_stop_requested = true;
    audio_cw_clear_pending_command();

    if (s_audio_task != NULL) {
        xTaskNotifyGive(s_audio_task);
    }

    ESP_LOGI(TAG, "stop all audio requested");
}

bool audio_cw_is_busy(void)
{
    return s_busy || s_command_pending;
}

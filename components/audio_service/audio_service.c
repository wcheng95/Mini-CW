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

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "audio_service";

#define AUDIO_CW_MIN_WPM 5
#define AUDIO_CW_MAX_WPM 40
#define AUDIO_CW_MIN_PITCH_HZ 300
#define AUDIO_CW_MAX_PITCH_HZ 999
#define AUDIO_CW_MAX_SAMPLE_RATE_HZ 48000U
#define AUDIO_CW_DEFAULT_SAMPLE_RATE_HZ 48000U
#define AUDIO_CW_TONE_CHUNK_MS 10
#define AUDIO_CW_ENVELOPE_MS 5
#define AUDIO_CW_STOP_SILENCE_MS 80
/*
 * Keyer tones (paddle elements and the straight-key release) must keep feeding
 * the codec until the I2S DMA has fully clocked out before muting, otherwise
 * the mute silences the ~30 ms still queued in the DMA (board_audio uses
 * dma_desc_num=6 * dma_frame_num=240 = 1440 frames = 30 ms at 48 kHz). That
 * truncates the raised-cosine release (clicks) and, at 30 WPM where a dit is
 * only 40 ms, swallows most of the element (noise). Drain with a margin over
 * the DMA depth. NOTE: validation value for the mute/drain-race hypothesis;
 * the real fix is continuous playback with an idle mute, which removes this
 * per-element silence entirely.
 */
#define AUDIO_CW_KEYER_DRAIN_MS 40
/*
 * Idle "gate": the codec output is muted only after this long with no audio,
 * never between elements. Muting/unmuting the ES8311 output stage pops, so
 * toggling it per dit/dah clicks on every key edge; holding it enabled across
 * a sending burst removes that click. Keep this longer than the gaps that
 * occur mid-sending (inter-word spacing, brief pauses) so it does not re-mute
 * in the middle of a QSO -- the only remaining edge is the first element after
 * a real idle, which costs one unmute pop in exchange for not draining the
 * battery while the rig sits unused.
 */
#define AUDIO_CW_IDLE_MUTE_MS 5000
#define AUDIO_CW_AMPLITUDE 12000
#define AUDIO_CW_TASK_STACK_BYTES 6144
#define AUDIO_CW_COMMAND_TEXT_MAX 1024
#define AUDIO_CW_COMMAND_PATTERN_MAX 16
#define AUDIO_SERVICE_DEFAULT_VOLUME_PERCENT 40
#define AUDIO_SERVICE_MAX_VOLUME_PERCENT 99
#define AUDIO_SERVICE_FEEDBACK_TONE_MS 50
/*
 * Sine sidetone DDS, ported from the Mini-FT8 qdx dds_q15 NCO: a 64-bit
 * free-running phase accumulator drives a 257-entry quarter-wave Q15 sine
 * table with 10-bit linear interpolation. The same quarter-wave table also
 * supplies the raised-cosine key envelope, because
 * (1 - cos(pi*t)) / 2 == sin^2(pi*t/2) and pi*t/2 spans exactly [0, pi/2].
 */
#define DDS_QTABLE_BITS 8u
#define DDS_FRAC_BITS 10u
#define DDS_QTABLE_SIZE (1u << DDS_QTABLE_BITS)
#define DDS_VISIBLE_BITS (2u + DDS_QTABLE_BITS + DDS_FRAC_BITS)
#define DDS_PHASE_BITS 64u

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
    AUDIO_CW_COMMAND_TONE_MS,
    AUDIO_CW_COMMAND_CONTINUOUS_TONE,
} audio_cw_command_type_t;

typedef struct {
    audio_cw_command_type_t type;
    char text[AUDIO_CW_COMMAND_TEXT_MAX];
    char ch;
    char pattern[AUDIO_CW_COMMAND_PATTERN_MAX];
    uint16_t duration_ms;
} audio_cw_command_t;

static const char *audio_cw_get_pattern(char ch);

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
    {'.', ".-.-.-"},
    {',', "--..--"},
    {'?', "..--.."},
    {'/', "-..-."},
    {'=', "-...-"},
};

static uint16_t s_pitch_hz = 700;
static uint8_t s_wpm = 16;
static uint8_t s_farnsworth_wpm = 12;
static uint8_t s_volume_percent = AUDIO_SERVICE_DEFAULT_VOLUME_PERCENT;
static bool s_output_ready;
static volatile bool s_busy;
static volatile bool s_stop_requested;
static volatile bool s_hard_stop_requested;
static volatile bool s_command_pending;
static volatile bool s_sidetone_latched;
static TaskHandle_t s_audio_task;
static SemaphoreHandle_t s_command_mutex;
static audio_cw_command_t s_pending_command;
static int16_t s_sin_quarter[DDS_QTABLE_SIZE + 1];
static uint64_t s_phase;
static volatile bool s_codec_active;

static audio_output_port_config_t s_output_config = {
    .sample_rate_hz = AUDIO_CW_DEFAULT_SAMPLE_RATE_HZ,
    .channels = 1,
    .bits_per_sample = 16,
    .volume_percent = AUDIO_SERVICE_DEFAULT_VOLUME_PERCENT,
};

static uint8_t clamp_percent(uint8_t percent)
{
    return percent > AUDIO_SERVICE_MAX_VOLUME_PERCENT ? AUDIO_SERVICE_MAX_VOLUME_PERCENT : percent;
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

static uint64_t audio_cw_phase_inc(void)
{
    /* inc = f * 2^64 / fs, matching dds_q15 inc_from_hz(). */
    long double num = (long double)s_pitch_hz * (long double)(1ULL << 63) * 2.0L;
    long double den = (long double)s_output_config.sample_rate_hz;

    if (den <= 0.0L) {
        return 0;
    }

    return (uint64_t)llroundl(num / den);
}

static void audio_cw_init_sine_lut(void)
{
    const double step = (M_PI / 2.0) / (double)DDS_QTABLE_SIZE;

    for (unsigned n = 0; n <= DDS_QTABLE_SIZE; ++n) {
        double s = sin(step * (double)n);
        int32_t q = (int32_t)lround(s * 32767.0);
        if (q > 32767) {
            q = 32767;
        }
        if (q < -32768) {
            q = -32768;
        }
        s_sin_quarter[n] = (int16_t)q;
    }
}

/* Sample sin(phase) from the quarter-wave LUT with linear interpolation. */
static inline int16_t audio_cw_sin_q15(uint64_t phase)
{
    uint32_t v = (uint32_t)(phase >> (DDS_PHASE_BITS - DDS_VISIBLE_BITS));
    uint32_t quad = v >> (DDS_QTABLE_BITS + DDS_FRAC_BITS);                  /* 0..3 */
    uint32_t xf = v & ((1u << (DDS_QTABLE_BITS + DDS_FRAC_BITS)) - 1u);
    uint32_t idx = xf >> DDS_FRAC_BITS;
    uint32_t frac = xf & ((1u << DDS_FRAC_BITS) - 1u);

    /* Mirror in odd quadrants (Q1, Q3): position = QTABLE_SIZE - position. */
    if (quad & 1u) {
        idx = DDS_QTABLE_SIZE - ((xf + ((1u << DDS_FRAC_BITS) - 1u)) >> DDS_FRAC_BITS);
        frac = (0u - frac) & ((1u << DDS_FRAC_BITS) - 1u);
    }

    int32_t y;
    if (idx >= DDS_QTABLE_SIZE) {
        y = s_sin_quarter[DDS_QTABLE_SIZE];
    } else {
        int32_t y0 = s_sin_quarter[idx];
        int32_t y1 = s_sin_quarter[idx + 1];
        int32_t dif = y1 - y0;
        int32_t acc = dif * (int32_t)frac;
        y = y0 + ((acc + (1 << (DDS_FRAC_BITS - 1))) >> DDS_FRAC_BITS);
    }

    if (quad >= 2u) {
        y = -y;
    }

    return (int16_t)y;
}

/*
 * Raised-cosine rising-edge gain (Q15) at sample i of an n-sample edge:
 * (1 - cos(pi*i/n)) / 2 == sin^2(pi*i/(2n)). The quarter-wave sine LUT spans
 * exactly [0, pi/2], so squaring its lookup yields the envelope without a
 * second table. A falling edge is the same curve indexed as (n - 1 - i), so
 * the last sample lands on exact silence, matching copy_trainer.py.
 */
static uint16_t audio_cw_rcos_rise_q15(uint32_t i, uint32_t n)
{
    if (n == 0 || i >= n) {
        return 32767;
    }

    uint32_t idx = (uint32_t)(((uint64_t)i * DDS_QTABLE_SIZE) / n);
    int32_t s = s_sin_quarter[idx];
    return (uint16_t)(((int32_t)s * s) >> 15);
}

/* Quarter-wave sine scaled by the Q15 key envelope and the tone amplitude. */
static int16_t audio_cw_render_sample(uint64_t phase, uint16_t env_q15)
{
    int32_t s = audio_cw_sin_q15(phase);
    s = (s * (int32_t)env_q15) >> 15;
    s = (s * AUDIO_CW_AMPLITUDE) >> 15;
    return (int16_t)s;
}

static uint32_t dit_ms(void)
{
    return 1200U / s_wpm;
}

static uint32_t farnsworth_dit_ms(void)
{
    uint8_t effective_wpm = s_farnsworth_wpm;

    if (effective_wpm == 0U || effective_wpm > s_wpm) {
        effective_wpm = s_wpm;
    }

    effective_wpm = clamp_wpm(effective_wpm);
    return 1200U / effective_wpm;
}

static TickType_t audio_cw_ms_to_delay_ticks(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return ticks > 0 ? ticks : 1;
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

static void audio_service_set_output_muted(bool muted)
{
    if (!s_output_ready) {
        return;
    }

    audio_output_port_set_muted(muted);
}

/*
 * Single owner of the codec mute state. Transition-guarded so the audio task
 * can call it before every element without re-toggling the output stage: the
 * codec is unmuted on the first element of a burst and stays enabled until the
 * idle gate (or a hard stop) releases it. All mute/unmute outside init goes
 * through here so s_codec_active always reflects the hardware.
 */
static void audio_cw_set_codec_active(bool active)
{
    if (s_codec_active == active) {
        return;
    }

    s_codec_active = active;
    audio_service_set_output_muted(!active);
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
        vTaskDelay(audio_cw_ms_to_delay_ticks(duration_ms));
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
            vTaskDelay(audio_cw_ms_to_delay_ticks((chunk_samples * 1000U) / sample_rate));
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
     * to samples using the active output sample rate. Very short tones cannot
     * fit a full attack plus release, so the envelope is scaled to the tone
     * length instead of exceeding it.
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

static uint16_t audio_cw_envelope_gain_q15(uint32_t sample_index,
                                           uint32_t total_samples,
                                           uint32_t attack_samples,
                                           uint32_t release_samples)
{
    uint16_t gain = 32767;

    if (total_samples == 0) {
        return 0;
    }

    if (attack_samples > 0 && sample_index < attack_samples) {
        gain = audio_cw_rcos_rise_q15(sample_index, attack_samples);
    }

    if (release_samples > 0) {
        const uint32_t release_start = total_samples - release_samples;
        if (sample_index >= release_start) {
            uint32_t pos_in_release = sample_index - release_start;
            uint16_t release_gain =
                audio_cw_rcos_rise_q15(release_samples - 1U - pos_in_release, release_samples);
            if (release_gain < gain) {
                gain = release_gain;
            }
        }
    }

    return gain;
}

static void audio_cw_gap_ms(uint32_t delay_ms)
{
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

    if (total_samples == 0 || max_chunk_samples == 0) {
        return;
    }

    const uint64_t phase_inc = audio_cw_phase_inc();
    uint64_t phase = s_phase;

    audio_cw_calculate_envelope_samples(total_samples,
                                        sample_rate,
                                        &attack_samples,
                                        &release_samples);

    /*
     * Pure sine sidetone from the shared quarter-wave DDS table with a
     * raised-cosine attack/release. The phase accumulator is seeded from the
     * persistent s_phase and never reset here, so consecutive dits/dahs stay
     * phase continuous; the envelope still returns to ~zero at both ends, so
     * element boundaries cannot click regardless of where phase lands.
     */
    while (samples_written < total_samples && !s_stop_requested) {
        uint32_t remaining = total_samples - samples_written;
        uint32_t chunk_samples = remaining < max_chunk_samples ? remaining : max_chunk_samples;

        for (uint32_t i = 0; i < chunk_samples; ++i) {
            uint32_t sample_index = samples_written + i;
            uint16_t envelope = audio_cw_envelope_gain_q15(sample_index,
                                                           total_samples,
                                                           attack_samples,
                                                           release_samples);
            samples[i] = audio_cw_render_sample(phase, envelope);
            phase += phase_inc;
        }

        size_t bytes_written = 0;
        bool wrote = s_output_ready &&
                     audio_output_port_write_pcm(samples, chunk_samples, &bytes_written);
        if (!wrote) {
            vTaskDelay(audio_cw_ms_to_delay_ticks((chunk_samples * 1000U) / sample_rate));
        }

        samples_written += chunk_samples;
    }

    s_phase = phase;
}

static void audio_cw_write_release_ramp(void)
{
    if (!audio_cw_sample_rate_supported(s_output_config.sample_rate_hz)) {
        return;
    }

    const uint32_t sample_rate = (uint32_t)s_output_config.sample_rate_hz;
    const uint32_t total_samples =
        (uint32_t)(((uint64_t)sample_rate * AUDIO_CW_ENVELOPE_MS) / 1000U);
    const uint32_t max_chunk_samples =
        (uint32_t)(((uint64_t)sample_rate * AUDIO_CW_TONE_CHUNK_MS) / 1000U);
    int16_t samples[(AUDIO_CW_MAX_SAMPLE_RATE_HZ * AUDIO_CW_TONE_CHUNK_MS) / 1000U];
    uint32_t samples_written = 0;

    if (total_samples == 0 || max_chunk_samples == 0) {
        return;
    }

    const uint64_t phase_inc = audio_cw_phase_inc();
    uint64_t phase = s_phase;

    while (samples_written < total_samples && !s_hard_stop_requested) {
        uint32_t remaining = total_samples - samples_written;
        uint32_t chunk_samples = remaining < max_chunk_samples ? remaining : max_chunk_samples;

        for (uint32_t i = 0; i < chunk_samples; ++i) {
            uint32_t sample_index = samples_written + i;
            uint16_t gain = audio_cw_rcos_rise_q15(total_samples - 1U - sample_index, total_samples);

            samples[i] = audio_cw_render_sample(phase, gain);
            phase += phase_inc;
        }

        size_t bytes_written = 0;
        bool wrote = s_output_ready &&
                     audio_output_port_write_pcm(samples, chunk_samples, &bytes_written);
        if (!wrote) {
            vTaskDelay(audio_cw_ms_to_delay_ticks((chunk_samples * 1000U) / sample_rate));
        }

        samples_written += chunk_samples;
    }

    s_phase = phase;
}

static void audio_cw_write_continuous_tone(void)
{
    if (!audio_cw_sample_rate_supported(s_output_config.sample_rate_hz)) {
        return;
    }

    const uint32_t sample_rate = (uint32_t)s_output_config.sample_rate_hz;
    const uint32_t chunk_samples =
        (uint32_t)(((uint64_t)sample_rate * AUDIO_CW_TONE_CHUNK_MS) / 1000U);
    uint32_t attack_samples =
        (uint32_t)(((uint64_t)sample_rate * AUDIO_CW_ENVELOPE_MS) / 1000U);
    int16_t samples[(AUDIO_CW_MAX_SAMPLE_RATE_HZ * AUDIO_CW_TONE_CHUNK_MS) / 1000U];
    uint64_t generated_samples = 0;

    if (chunk_samples == 0) {
        return;
    }

    if (attack_samples == 0) {
        attack_samples = 1;
    }

    const uint64_t phase_inc = audio_cw_phase_inc();
    uint64_t phase = s_phase;

    while (!s_stop_requested) {
        for (uint32_t i = 0; i < chunk_samples; ++i) {
            uint16_t gain = (generated_samples < attack_samples)
                                ? audio_cw_rcos_rise_q15((uint32_t)generated_samples, attack_samples)
                                : 32767;

            samples[i] = audio_cw_render_sample(phase, gain);
            phase += phase_inc;
            ++generated_samples;
        }

        size_t bytes_written = 0;
        bool wrote = s_output_ready &&
                     audio_output_port_write_pcm(samples, chunk_samples, &bytes_written);
        if (!wrote) {
            vTaskDelay(audio_cw_ms_to_delay_ticks(AUDIO_CW_TONE_CHUNK_MS));
        }
    }

    s_phase = phase;

    if (!s_hard_stop_requested) {
        audio_cw_write_release_ramp();
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
            audio_cw_gap_ms(unit_ms);
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
        ESP_LOGI(TAG, "character gap: %u ms", (unsigned)(farnsworth_dit_ms() * 3U));
        audio_cw_gap_ms(farnsworth_dit_ms() * 3U);
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
            ESP_LOGI(TAG, "word gap: %u ms", (unsigned)(farnsworth_dit_ms() * 7U));
            audio_cw_gap_ms(farnsworth_dit_ms() * 7U);
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
            ESP_LOGI(TAG, "character gap: %u ms", (unsigned)(farnsworth_dit_ms() * 3U));
            audio_cw_gap_ms(farnsworth_dit_ms() * 3U);
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
    case AUDIO_CW_COMMAND_TONE_MS:
        audio_cw_write_tone_ms(command->duration_ms);
        break;
    case AUDIO_CW_COMMAND_CONTINUOUS_TONE:
        audio_cw_write_continuous_tone();
        break;
    case AUDIO_CW_COMMAND_NONE:
    default:
        break;
    }
}

static uint32_t audio_cw_command_stop_silence_ms(const audio_cw_command_t *command)
{
    if (command == NULL) {
        return AUDIO_CW_STOP_SILENCE_MS;
    }

    switch (command->type) {
    case AUDIO_CW_COMMAND_TONE_MS:
    case AUDIO_CW_COMMAND_CONTINUOUS_TONE:
        return AUDIO_CW_KEYER_DRAIN_MS;
    case AUDIO_CW_COMMAND_TEXT:
    case AUDIO_CW_COMMAND_CHAR:
    case AUDIO_CW_COMMAND_PATTERN:
    case AUDIO_CW_COMMAND_FEEDBACK_TONE:
    case AUDIO_CW_COMMAND_NONE:
    default:
        return AUDIO_CW_STOP_SILENCE_MS;
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
        /*
         * While the codec is enabled, wake after the idle timeout so silence
         * can release it (the gate). While it is already muted there is nothing
         * to release, so block until the next command. The codec is muted only
         * on this idle path, never between elements, so key edges cannot click.
         */
        uint32_t notified = ulTaskNotifyTake(pdTRUE,
                                             s_codec_active
                                                 ? pdMS_TO_TICKS(AUDIO_CW_IDLE_MUTE_MS)
                                                 : portMAX_DELAY);

        if (notified == 0) {
            audio_cw_set_codec_active(false);
            continue;
        }

        for (;;) {
            audio_cw_command_t command;
            if (!audio_cw_take_pending_command(&command)) {
                break;
            }

            s_stop_requested = false;
            s_hard_stop_requested = false;
            s_busy = true;
            audio_cw_set_codec_active(true);
            audio_cw_run_command(&command);
            uint32_t stop_silence_ms = audio_cw_command_stop_silence_ms(&command);
            if (stop_silence_ms > 0) {
                audio_cw_write_silence_ms(stop_silence_ms, false);
            }
            if (command.type == AUDIO_CW_COMMAND_CONTINUOUS_TONE) {
                s_sidetone_latched = false;
            }
            s_busy = false;
        }
    }
}

static void audio_cw_queue_command(const audio_cw_command_t *command)
{
    if (command == NULL) {
        return;
    }

    s_hard_stop_requested = true;
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
    s_hard_stop_requested = false;
    s_busy = true;
    audio_cw_set_codec_active(true);
    audio_cw_run_command(command);
    uint32_t stop_silence_ms = audio_cw_command_stop_silence_ms(command);
    if (stop_silence_ms > 0) {
        audio_cw_write_silence_ms(stop_silence_ms, false);
    }
    audio_cw_set_codec_active(false);
    if (command->type == AUDIO_CW_COMMAND_CONTINUOUS_TONE) {
        s_sidetone_latched = false;
    }
    s_busy = false;
}

void audio_service_init(void)
{
    audio_cw_init_sine_lut();
    s_output_config.volume_percent = s_volume_percent;
    s_wpm = clamp_wpm(s_wpm);
    s_pitch_hz = clamp_pitch(s_pitch_hz);
    s_output_ready = audio_output_port_init(&s_output_config);
    audio_service_set_output_muted(true);

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

    if (next > AUDIO_SERVICE_MAX_VOLUME_PERCENT) {
        next = AUDIO_SERVICE_MAX_VOLUME_PERCENT;
    }

    audio_service_set_volume((uint8_t)next);
}

void audio_service_play_feedback_beep(void)
{
    audio_cw_command_t command = {
        .type = AUDIO_CW_COMMAND_FEEDBACK_TONE,
    };

    ESP_LOGI(TAG, "queue feedback tone: %u ms", (unsigned)AUDIO_SERVICE_FEEDBACK_TONE_MS);
    audio_cw_queue_command(&command);
}

void audio_service_play_feedback_tone(void)
{
    audio_service_play_feedback_beep();
}

void audio_service_tone_on(void)
{
    if (s_sidetone_latched) {
        return;
    }

    if (s_audio_task == NULL) {
        ESP_LOGW(TAG, "continuous sidetone unavailable without audio task");
        return;
    }

    audio_cw_command_t command = {
        .type = AUDIO_CW_COMMAND_CONTINUOUS_TONE,
    };

    s_sidetone_latched = true;
    ESP_LOGI(TAG, "sidetone on");
    audio_cw_queue_command(&command);
}

void audio_service_tone_off(void)
{
    if (!s_sidetone_latched) {
        return;
    }

    s_sidetone_latched = false;
    ESP_LOGI(TAG, "sidetone off");
    s_hard_stop_requested = false;
    s_stop_requested = true;
    audio_cw_clear_pending_command();

    if (s_audio_task != NULL) {
        xTaskNotifyGive(s_audio_task);
    }
}

static void audio_service_play_keyer_tone(uint16_t duration_ms, const char *label)
{
    if (duration_ms == 0) {
        return;
    }

    audio_cw_command_t command = {
        .type = AUDIO_CW_COMMAND_TONE_MS,
        .duration_ms = duration_ms,
    };

    ESP_LOGI(TAG, "queue keyer %s: %u ms", label, (unsigned)duration_ms);
    audio_cw_queue_command(&command);
}

void audio_service_play_dit(uint16_t dit_ms)
{
    audio_service_play_keyer_tone(dit_ms, "dit");
}

void audio_service_play_dah(uint16_t dit_ms)
{
    audio_service_play_keyer_tone((uint16_t)(3U * dit_ms), "dah");
}

static void audio_cw_set_wpm(uint8_t wpm)
{
    s_wpm = clamp_wpm(wpm);
    ESP_LOGI(TAG, "set wpm: %u", (unsigned)s_wpm);
}

static void audio_cw_set_farnsworth_wpm(uint8_t effective_wpm)
{
    s_farnsworth_wpm = clamp_wpm(effective_wpm);
    if (s_farnsworth_wpm > s_wpm) {
        s_farnsworth_wpm = s_wpm;
    }
    ESP_LOGI(TAG, "set farnsworth wpm: %u", (unsigned)s_farnsworth_wpm);
}

static uint8_t audio_cw_get_wpm(void)
{
    return s_wpm;
}

static uint8_t audio_cw_get_farnsworth_wpm(void)
{
    return s_farnsworth_wpm;
}

static const char *audio_cw_get_pattern(char ch)
{
    char normalized = (char)toupper((unsigned char)ch);
    for (size_t i = 0; i < sizeof(MORSE_TABLE) / sizeof(MORSE_TABLE[0]); ++i) {
        if (MORSE_TABLE[i].ch == normalized) {
            return MORSE_TABLE[i].pattern;
        }
    }

    return NULL;
}

static void audio_cw_play_text(const char *text)
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

static void audio_cw_play_char(char ch)
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

static void audio_cw_play_pattern(const char *pattern)
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

static void audio_cw_play_symbol(char symbol)
{
    audio_cw_play_char(symbol);
}

static void audio_cw_stop(void)
{
    s_sidetone_latched = false;
    s_hard_stop_requested = true;
    s_stop_requested = true;
    audio_cw_clear_pending_command();
    audio_cw_set_codec_active(false);

    if (s_audio_task != NULL) {
        xTaskNotifyGive(s_audio_task);
    }

    ESP_LOGI(TAG, "stop all audio requested");
}

static bool audio_cw_is_busy(void)
{
    return s_busy || s_command_pending;
}

void audio_service_stop_all(void)
{
    audio_cw_stop();
}

bool audio_service_is_busy(void)
{
    return audio_cw_is_busy();
}

void audio_service_set_cw_wpm(uint8_t wpm)
{
    audio_cw_set_wpm(wpm);
}

uint8_t audio_service_get_cw_wpm(void)
{
    return audio_cw_get_wpm();
}

void audio_service_set_cw_farnsworth_wpm(uint8_t effective_wpm)
{
    audio_cw_set_farnsworth_wpm(effective_wpm);
}

uint8_t audio_service_get_cw_farnsworth_wpm(void)
{
    return audio_cw_get_farnsworth_wpm();
}

const char *audio_service_get_cw_pattern(char ch)
{
    return audio_cw_get_pattern(ch);
}

void audio_service_play_cw_text(const char *text)
{
    audio_cw_play_text(text);
}

void audio_service_play_cw_char(char ch)
{
    audio_cw_play_char(ch);
}

void audio_service_play_cw_pattern(const char *pattern)
{
    audio_cw_play_pattern(pattern);
}

void audio_service_play_cw_symbol(char symbol)
{
    audio_cw_play_symbol(symbol);
}

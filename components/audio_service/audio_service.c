/*
 * audio_service
 *
 * Responsibility: Owns all speaker and CW tone output.
 * Hardware ownership: speaker/tone output. A single audio task streams PCM to
 * the codec continuously (zeros = silence) and never mutes between elements;
 * callers drive it through a segment FIFO.
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
/*
 * Render granularity. The stream loop re-checks the segment ring / flush flag
 * and refills text once per chunk, so the chunk period bounds preemption and
 * keying latency; 5 ms matches the 5 ms app/keyer poll.
 */
#define AUDIO_CW_TONE_CHUNK_MS 5
#define AUDIO_CW_ENVELOPE_MS 5
#define AUDIO_CW_AMPLITUDE 12000
#define AUDIO_CW_TASK_STACK_BYTES 6144
#define AUDIO_CW_TEXT_MAX 1024
#define AUDIO_SERVICE_DEFAULT_VOLUME_PERCENT 40
#define AUDIO_SERVICE_MAX_VOLUME_PERCENT 99
#define AUDIO_SERVICE_FEEDBACK_TONE_MS 50
/*
 * Segment FIFO. Producers push a few segments at a time; text is expanded one
 * character at a time on the audio task so a long lesson never needs the whole
 * message pre-expanded. The longest Morse symbol is 6 elements -> 6 tones + 5
 * element gaps + 1 inter-character gap = 12 segments, so the loop only expands
 * a character when at least that many ring slots are free.
 */
#define AUDIO_SEG_RING_CAP 64
#define AUDIO_SEG_MAX_PER_CHAR 12
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

/*
 * One unit of work for the stream loop. A keyed element (SEG_TONE) carries its
 * own raised-cosine attack/release flags; gaps are SEG_SILENCE; the straight
 * key holds at full gain (SEG_TONE_HOLD) until a release is requested.
 */
typedef enum {
    SEG_SILENCE = 0,
    SEG_TONE,
    SEG_TONE_HOLD,
} seg_kind_t;

typedef struct {
    seg_kind_t kind;
    uint32_t duration_samples;
    bool attack;
    bool release;
} audio_seg_t;

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
static TaskHandle_t s_audio_task;
static SemaphoreHandle_t s_seg_mutex;
static int16_t s_sin_quarter[DDS_QTABLE_SIZE + 1];
static uint64_t s_phase;

/* Segment ring (guarded by s_seg_mutex). */
static audio_seg_t s_seg_ring[AUDIO_SEG_RING_CAP];
static uint16_t s_seg_head;
static uint16_t s_seg_tail;
static uint16_t s_seg_count;

/* Incremental text source (guarded by s_seg_mutex). */
static char s_text_src[AUDIO_CW_TEXT_MAX + 1U];
static uint16_t s_text_pos;
static volatile bool s_text_active;

/* Stream control flags (guarded by s_seg_mutex unless noted). */
static volatile bool s_flush_requested;   /* ramp current tone down, then drop */
static volatile bool s_hold_active;       /* a straight-key hold is in flight */

/* Render-loop-private cursor (touched only by the audio task). */
static audio_seg_t s_cur;
static bool s_cur_valid;
static uint32_t s_cur_pos;
static uint32_t s_cur_attack_n;
static uint32_t s_cur_release_n;
static bool s_rel_mode;        /* current segment is a flush/release tail */
static uint16_t s_rel_g0;      /* gain the release tail starts from */
static uint32_t s_cur_gen;     /* busy generation the cursor's tone belongs to */

/* is_busy() source of truth: keyed samples enqueued but not yet emitted. */
static portMUX_TYPE s_busy_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_keyed_outstanding;
static volatile uint32_t s_busy_gen;

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

static uint32_t audio_cw_ms_to_samples(uint32_t ms)
{
    return (uint32_t)(((uint64_t)s_output_config.sample_rate_hz * ms) / 1000U);
}

static TickType_t audio_cw_ms_to_delay_ticks(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return ticks > 0 ? ticks : 1;
}

static bool audio_cw_lock(TickType_t timeout)
{
    if (s_seg_mutex == NULL) {
        return true;
    }

    return xSemaphoreTake(s_seg_mutex, timeout) == pdTRUE;
}

static void audio_cw_unlock(void)
{
    if (s_seg_mutex != NULL) {
        xSemaphoreGive(s_seg_mutex);
    }
}

static bool audio_cw_sample_rate_supported(int sample_rate_hz)
{
    /*
     * The CW tone generator currently supports output rates up to 48 kHz. Its
     * local stack PCM buffer is sized from AUDIO_CW_MAX_SAMPLE_RATE_HZ, so a
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
 * is_busy() accounting: keyed (tone) samples that have been enqueued but not
 * yet emitted into a written chunk. Producers add synchronously so is_busy()
 * is true the instant play_dit/dah returns; the render loop subtracts as it
 * commits keyed samples, so it goes false no earlier than the element is sent.
 */
static uint32_t audio_cw_busy_generation(void)
{
    return s_busy_gen;
}

static void audio_cw_busy_add(uint32_t samples)
{
    portENTER_CRITICAL(&s_busy_mux);
    s_keyed_outstanding += samples;
    portEXIT_CRITICAL(&s_busy_mux);
}

/*
 * Subtract emitted keyed samples, but only if no preempt cleared the counter
 * since this tally's element was loaded into the cursor. A flush bumps the
 * generation under the same spinlock as the replacement element's busy_add, so
 * a tally accumulated for a since-preempted element fails the match and is
 * dropped instead of being charged against the new element's fresh count.
 */
static void audio_cw_busy_sub(uint32_t samples, uint32_t gen)
{
    portENTER_CRITICAL(&s_busy_mux);
    if (s_busy_gen == gen) {
        s_keyed_outstanding = (s_keyed_outstanding > samples) ? (s_keyed_outstanding - samples) : 0U;
    }
    portEXIT_CRITICAL(&s_busy_mux);
}

static void audio_cw_busy_clear(void)
{
    portENTER_CRITICAL(&s_busy_mux);
    s_keyed_outstanding = 0U;
    s_busy_gen++;
    portEXIT_CRITICAL(&s_busy_mux);
}

/* Ring helpers; caller must hold s_seg_mutex. */
static bool audio_cw_seg_push(const audio_seg_t *seg)
{
    if (s_seg_count >= AUDIO_SEG_RING_CAP) {
        return false;
    }

    s_seg_ring[s_seg_tail] = *seg;
    s_seg_tail = (uint16_t)((s_seg_tail + 1U) % AUDIO_SEG_RING_CAP);
    s_seg_count++;
    return true;
}

static bool audio_cw_seg_pop(audio_seg_t *out)
{
    if (s_seg_count == 0) {
        return false;
    }

    *out = s_seg_ring[s_seg_head];
    s_seg_head = (uint16_t)((s_seg_head + 1U) % AUDIO_SEG_RING_CAP);
    s_seg_count--;
    return true;
}

static void audio_cw_seg_clear(void)
{
    s_seg_head = 0;
    s_seg_tail = 0;
    s_seg_count = 0;
}

static uint16_t audio_cw_seg_free(void)
{
    return (uint16_t)(AUDIO_SEG_RING_CAP - s_seg_count);
}

/* Drop everything queued and ask the loop to ramp the live tone down cleanly. */
static void audio_cw_preempt_locked(void)
{
    audio_cw_seg_clear();
    s_text_active = false;
    s_hold_active = false;
    audio_cw_busy_clear();
    s_flush_requested = true;
}

/* Push a finite keyed tone of N samples (counts toward is_busy). */
static void audio_cw_push_tone_locked(uint32_t samples)
{
    if (samples == 0) {
        return;
    }

    audio_seg_t seg = {
        .kind = SEG_TONE,
        .duration_samples = samples,
        .attack = true,
        .release = true,
    };

    if (audio_cw_seg_push(&seg)) {
        audio_cw_busy_add(samples);
    } else {
        ESP_LOGW(TAG, "segment ring full; dropping tone");
    }
}

static void audio_cw_push_silence_locked(uint32_t samples)
{
    if (samples == 0) {
        return;
    }

    audio_seg_t seg = {
        .kind = SEG_SILENCE,
        .duration_samples = samples,
    };
    audio_cw_seg_push(&seg);
}

/* Gain the current cursor would emit at s_cur_pos right now. */
static uint16_t audio_cw_cursor_gain(void)
{
    if (s_rel_mode) {
        uint16_t fall = audio_cw_rcos_rise_q15(s_cur_release_n - 1U - s_cur_pos, s_cur_release_n);
        return (uint16_t)(((uint32_t)s_rel_g0 * fall) >> 15);
    }

    if (s_cur.kind == SEG_TONE_HOLD) {
        return (s_cur_pos < s_cur_attack_n)
                   ? audio_cw_rcos_rise_q15(s_cur_pos, s_cur_attack_n)
                   : 32767;
    }

    return audio_cw_envelope_gain_q15(s_cur_pos,
                                      s_cur.duration_samples,
                                      s_cur_attack_n,
                                      s_cur_release_n);
}

/* Convert whatever is sounding into a short raised-cosine release tail. */
static void audio_cw_begin_release_locked(void)
{
    if (!s_cur_valid || s_cur.kind == SEG_SILENCE) {
        s_cur_valid = false;
        s_rel_mode = false;
        return;
    }

    uint16_t g0 = audio_cw_cursor_gain();
    uint32_t rel = audio_cw_ms_to_samples(AUDIO_CW_ENVELOPE_MS);
    if (rel == 0) {
        rel = 1;
    }

    s_rel_g0 = g0;
    s_cur.kind = SEG_TONE;
    s_cur.duration_samples = rel;
    s_cur.attack = false;
    s_cur.release = true;
    s_cur_pos = 0;
    s_cur_attack_n = 0;
    s_cur_release_n = rel;
    s_rel_mode = true;
    s_cur_valid = true;
}

/* Pop the next segment into the cursor; returns false if the ring is empty. */
static bool audio_cw_refill_cursor(void)
{
    audio_seg_t seg;
    bool got = false;

    if (audio_cw_lock(portMAX_DELAY)) {
        got = audio_cw_seg_pop(&seg);
        audio_cw_unlock();
    }

    if (!got) {
        return false;
    }

    s_cur = seg;
    s_cur_pos = 0;
    s_rel_mode = false;
    s_cur_gen = audio_cw_busy_generation();

    if (seg.kind == SEG_TONE) {
        uint32_t attack_n = 0;
        uint32_t release_n = 0;
        audio_cw_calculate_envelope_samples(seg.duration_samples,
                                            (uint32_t)s_output_config.sample_rate_hz,
                                            &attack_n,
                                            &release_n);
        s_cur_attack_n = seg.attack ? attack_n : 0;
        s_cur_release_n = seg.release ? release_n : 0;
    } else if (seg.kind == SEG_TONE_HOLD) {
        uint32_t env = audio_cw_ms_to_samples(AUDIO_CW_ENVELOPE_MS);
        s_cur_attack_n = env > 0 ? env : 1;
        s_cur_release_n = 0;
    }

    s_cur_valid = true;
    return true;
}

/* Expand one source character into segments; caller holds s_seg_mutex. */
static bool audio_cw_expand_next_char_locked(void)
{
    char ch = s_text_src[s_text_pos];
    if (ch == '\0') {
        return false;
    }

    if (ch == ' ') {
        audio_cw_push_silence_locked(audio_cw_ms_to_samples(farnsworth_dit_ms() * 7U));
        s_text_pos++;
        return true;
    }

    const char *pattern = audio_cw_get_pattern(ch);
    if (pattern != NULL) {
        uint32_t unit = audio_cw_ms_to_samples(dit_ms());
        for (const char *p = pattern; *p != '\0'; ++p) {
            uint32_t units = (*p == '-') ? 3U : 1U;
            audio_cw_push_tone_locked(unit * units);
            if (p[1] != '\0') {
                audio_cw_push_silence_locked(unit); /* element gap */
            }
        }

        char next = s_text_src[s_text_pos + 1];
        if (next != '\0' && next != ' ') {
            audio_cw_push_silence_locked(audio_cw_ms_to_samples(farnsworth_dit_ms() * 3U));
        }
    } else {
        ESP_LOGW(TAG, "unsupported text character: '%c'", ch);
    }

    s_text_pos++;
    return true;
}

static void audio_cw_task(void *arg)
{
    (void)arg;

    const uint32_t sample_rate = (uint32_t)s_output_config.sample_rate_hz;
    const uint32_t chunk = audio_cw_ms_to_samples(AUDIO_CW_TONE_CHUNK_MS);
    int16_t buf[(AUDIO_CW_MAX_SAMPLE_RATE_HZ * AUDIO_CW_TONE_CHUNK_MS) / 1000U];

    if (chunk == 0 || !audio_cw_sample_rate_supported((int)sample_rate)) {
        ESP_LOGE(TAG, "unsupported sample rate %u; audio task idle", (unsigned)sample_rate);
        vTaskDelete(NULL);
        return;
    }

    /*
     * Prime the DMA with silence while still muted, then unmute so the codec
     * turns on with zeros already flowing -> no startup pop. From here the
     * codec stays enabled; the stream feeds zeros when idle.
     */
    memset(buf, 0, chunk * sizeof(int16_t));
    for (int k = 0; k < 8; ++k) {
        size_t bw = 0;
        if (!(s_output_ready && audio_output_port_write_pcm(buf, chunk, &bw))) {
            break;
        }
    }
    audio_service_set_output_muted(false);

    for (;;) {
        const uint64_t phase_inc = audio_cw_phase_inc();

        if (audio_cw_lock(portMAX_DELAY)) {
            if (s_flush_requested) {
                s_flush_requested = false;
                audio_cw_begin_release_locked();
            }
            while (s_text_active && audio_cw_seg_free() >= AUDIO_SEG_MAX_PER_CHAR) {
                if (!audio_cw_expand_next_char_locked()) {
                    s_text_active = false;
                    break;
                }
            }
            audio_cw_unlock();
        }

        uint32_t keyed_tally = 0;
        uint32_t keyed_gen = 0;
        uint32_t i = 0;
        while (i < chunk) {
            if (!s_cur_valid && !audio_cw_refill_cursor()) {
                /* Ring empty: the rest of this chunk is silence. */
                while (i < chunk) {
                    buf[i++] = 0;
                }
                break;
            }

            if (s_cur.kind == SEG_SILENCE) {
                buf[i] = 0;
                s_cur_pos++;
                if (s_cur_pos >= s_cur.duration_samples) {
                    s_cur_valid = false;
                }
            } else {
                uint16_t gain = audio_cw_cursor_gain();
                buf[i] = audio_cw_render_sample(s_phase, gain);
                s_phase += phase_inc;

                if (s_cur.kind == SEG_TONE && !s_rel_mode) {
                    if (keyed_tally == 0) {
                        keyed_gen = s_cur_gen;
                    }
                    keyed_tally++;
                }

                if (s_cur.kind == SEG_TONE_HOLD) {
                    if (s_cur_pos < s_cur_attack_n) {
                        s_cur_pos++; /* freeze position once the attack completes */
                    }
                } else {
                    s_cur_pos++;
                    if (s_cur_pos >= s_cur.duration_samples) {
                        s_rel_mode = false;
                        s_cur_valid = false;
                    }
                }
            }

            i++;
        }

        size_t bytes_written = 0;
        if (!(s_output_ready &&
              audio_output_port_write_pcm(buf, chunk, &bytes_written))) {
            vTaskDelay(audio_cw_ms_to_delay_ticks(AUDIO_CW_TONE_CHUNK_MS));
        }

        /*
         * Subtract after the chunk is committed so is_busy() tracks
         * enqueued-to-DMA, not merely rendered. The generation guard drops the
         * tally if a preempt reset the counter for a replacement element while
         * this chunk was in flight.
         */
        if (keyed_tally > 0) {
            audio_cw_busy_sub(keyed_tally, keyed_gen);
        }
    }
}

void audio_service_init(void)
{
    audio_cw_init_sine_lut();
    s_output_config.volume_percent = s_volume_percent;
    s_wpm = clamp_wpm(s_wpm);
    s_pitch_hz = clamp_pitch(s_pitch_hz);
    s_output_ready = audio_output_port_init(&s_output_config);
    audio_service_set_output_muted(true);

    if (s_seg_mutex == NULL) {
        s_seg_mutex = xSemaphoreCreateMutex();
        if (s_seg_mutex == NULL) {
            ESP_LOGW(TAG, "failed to create audio segment mutex");
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
    ESP_LOGI(TAG, "CW playback: continuous PCM stream, dit=1200/WPM");
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
    if (audio_cw_lock(pdMS_TO_TICKS(20))) {
        audio_cw_preempt_locked();
        audio_cw_push_tone_locked(audio_cw_ms_to_samples(AUDIO_SERVICE_FEEDBACK_TONE_MS));
        audio_cw_unlock();
    }
}

void audio_service_play_feedback_tone(void)
{
    audio_service_play_feedback_beep();
}

void audio_service_tone_on(void)
{
    if (s_hold_active) {
        return;
    }

    if (audio_cw_lock(pdMS_TO_TICKS(20))) {
        audio_cw_seg_clear();
        s_text_active = false;
        audio_cw_busy_clear();
        audio_seg_t seg = {.kind = SEG_TONE_HOLD, .attack = true};
        audio_cw_seg_push(&seg);
        s_hold_active = true;
        s_flush_requested = true; /* ramp down any leftover before the hold */
        audio_cw_unlock();
        ESP_LOGI(TAG, "sidetone on");
    }
}

void audio_service_tone_off(void)
{
    if (!s_hold_active) {
        return;
    }

    if (audio_cw_lock(pdMS_TO_TICKS(20))) {
        audio_cw_seg_clear();
        s_hold_active = false;
        audio_cw_busy_clear();
        s_flush_requested = true; /* ramp the hold down */
        audio_cw_unlock();
        ESP_LOGI(TAG, "sidetone off");
    }
}

void audio_service_play_dit(uint16_t dit_ms_arg)
{
    if (dit_ms_arg == 0) {
        return;
    }

    if (audio_cw_lock(pdMS_TO_TICKS(20))) {
        audio_cw_push_tone_locked(audio_cw_ms_to_samples(dit_ms_arg));
        audio_cw_unlock();
    }
}

void audio_service_play_dah(uint16_t dit_ms_arg)
{
    audio_service_play_dit((uint16_t)(3U * dit_ms_arg));
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

static void audio_cw_start_text(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }

    if (audio_cw_lock(pdMS_TO_TICKS(20))) {
        audio_cw_preempt_locked();
        strncpy(s_text_src, text, sizeof(s_text_src) - 1U);
        s_text_src[sizeof(s_text_src) - 1U] = '\0';
        s_text_pos = 0;
        s_text_active = (s_text_src[0] != '\0');
        audio_cw_unlock();
        ESP_LOGI(TAG, "play text: %s", s_text_src);
    }
}

void audio_service_play_cw_text(const char *text)
{
    audio_cw_start_text(text);
}

void audio_service_play_cw_char(char ch)
{
    char buf[2] = {ch, '\0'};

    if (audio_cw_get_pattern(ch) == NULL) {
        ESP_LOGW(TAG, "unsupported character: '%c'", ch);
        return;
    }

    audio_cw_start_text(buf);
}

void audio_service_play_cw_pattern(const char *pattern)
{
    if (pattern == NULL || pattern[0] == '\0') {
        return;
    }

    if (audio_cw_lock(pdMS_TO_TICKS(20))) {
        audio_cw_preempt_locked();
        uint32_t unit = audio_cw_ms_to_samples(dit_ms());
        for (const char *p = pattern; *p != '\0'; ++p) {
            uint32_t units = (*p == '-') ? 3U : (*p == '.') ? 1U : 0U;
            if (units == 0) {
                continue;
            }
            audio_cw_push_tone_locked(unit * units);
            if (p[1] != '\0') {
                audio_cw_push_silence_locked(unit);
            }
        }
        audio_cw_unlock();
    }
}

void audio_service_play_cw_symbol(char symbol)
{
    audio_service_play_cw_char(symbol);
}

void audio_service_stop_all(void)
{
    if (audio_cw_lock(pdMS_TO_TICKS(20))) {
        audio_cw_preempt_locked();
        audio_cw_unlock();
    }

    ESP_LOGI(TAG, "stop all audio requested");
}

bool audio_service_is_busy(void)
{
    if (s_audio_task == NULL) {
        return false;
    }

    return s_keyed_outstanding != 0U || s_hold_active || s_text_active;
}

void audio_service_set_cw_wpm(uint8_t wpm)
{
    audio_cw_set_wpm(wpm);
}

uint8_t audio_service_get_cw_wpm(void)
{
    return s_wpm;
}

void audio_service_set_cw_farnsworth_wpm(uint8_t effective_wpm)
{
    audio_cw_set_farnsworth_wpm(effective_wpm);
}

uint8_t audio_service_get_cw_farnsworth_wpm(void)
{
    return s_farnsworth_wpm;
}

const char *audio_service_get_cw_pattern(char ch)
{
    return audio_cw_get_pattern(ch);
}

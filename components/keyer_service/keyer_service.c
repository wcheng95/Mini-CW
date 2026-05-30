/*
 * keyer_service
 *
 * Responsibility: Owns paddle/key input abstraction and emits decoded keyer
 * events for TX practice.
 * Hardware ownership: paddle/key GPIO. Milestone 1 does not read GPIO yet.
 * Future sidetone must be requested through audio_service, not produced here.
 */

#include "keyer_service.h"

#include "esp_log.h"

static const char *TAG = "keyer_service";

#define KEYER_DEFAULT_TX_WPM 20U
#define KEYER_MIN_TX_WPM 5U
#define KEYER_MAX_TX_WPM 60U
#define KEYER_IO_MODE_COUNT 4

static const keyer_event_t KEYER_NO_EVENT = {
    .type = KEYER_EVENT_NONE,
    .decoded_char = '\0',
    .duration_ms = 0,
};

static keyer_io_mode_t s_key_in_mode = KEYER_IO_PADDLE;
static keyer_io_mode_t s_key_out_mode = KEYER_IO_PADDLE;
static uint8_t s_key_in_wpm = KEYER_DEFAULT_TX_WPM;
static uint8_t s_key_out_wpm = KEYER_DEFAULT_TX_WPM;

static uint16_t keyer_clamp_tx_wpm(uint16_t wpm)
{
    if (wpm < KEYER_MIN_TX_WPM) {
        return KEYER_MIN_TX_WPM;
    }

    if (wpm > KEYER_MAX_TX_WPM) {
        return KEYER_MAX_TX_WPM;
    }

    return wpm;
}

static const char *keyer_input_mode_name(keyer_input_mode_t mode)
{
    switch (mode) {
    case KEYER_INPUT_STRAIGHT_KEY:
        return "straight key";
    case KEYER_INPUT_SINGLE_PADDLE:
        return "single paddle";
    case KEYER_INPUT_DUAL_PADDLE:
        return "dual paddle";
    default:
        return "unknown";
    }
}

static keyer_io_mode_t keyer_clamp_io_mode(keyer_io_mode_t mode)
{
    if ((int)mode < 0 || (int)mode >= KEYER_IO_MODE_COUNT) {
        return KEYER_IO_PADDLE;
    }

    return mode;
}

static keyer_io_mode_t keyer_cycle_io_mode(keyer_io_mode_t mode, int direction)
{
    int next = (int)keyer_clamp_io_mode(mode);

    if (direction < 0) {
        --next;
    } else if (direction > 0) {
        ++next;
    }

    if (next < 0) {
        next = KEYER_IO_MODE_COUNT - 1;
    } else if (next >= KEYER_IO_MODE_COUNT) {
        next = 0;
    }

    return (keyer_io_mode_t)next;
}

void keyer_service_init(void)
{
    ESP_LOGI(TAG, "initialized paddle/key owner (Milestone 1 stub)");
    ESP_LOGI(TAG,
             "key in=%s key out=%s key_in_wpm=%u key_out_wpm=%u",
             keyer_service_io_mode_label(s_key_in_mode),
             keyer_service_io_mode_label(s_key_out_mode),
             (unsigned)s_key_in_wpm,
             (unsigned)s_key_out_wpm);
}

keyer_io_mode_t keyer_service_get_key_in_mode(void)
{
    return s_key_in_mode;
}

void keyer_service_set_key_in_mode(keyer_io_mode_t mode)
{
    s_key_in_mode = keyer_clamp_io_mode(mode);
    ESP_LOGI(TAG, "key in mode: %s", keyer_service_io_mode_label(s_key_in_mode));
}

void keyer_service_cycle_key_in_mode(int direction)
{
    keyer_service_set_key_in_mode(keyer_cycle_io_mode(s_key_in_mode, direction));
}

keyer_io_mode_t keyer_service_get_key_out_mode(void)
{
    return s_key_out_mode;
}

void keyer_service_set_key_out_mode(keyer_io_mode_t mode)
{
    s_key_out_mode = keyer_clamp_io_mode(mode);
    ESP_LOGI(TAG, "key out mode: %s", keyer_service_io_mode_label(s_key_out_mode));
}

void keyer_service_cycle_key_out_mode(int direction)
{
    keyer_service_set_key_out_mode(keyer_cycle_io_mode(s_key_out_mode, direction));
}

uint8_t keyer_service_get_key_in_wpm(void)
{
    return s_key_in_wpm;
}

void keyer_service_set_key_in_wpm(uint8_t wpm)
{
    s_key_in_wpm = (uint8_t)keyer_clamp_tx_wpm(wpm);
    ESP_LOGI(TAG, "key in wpm: %u", (unsigned)s_key_in_wpm);
}

void keyer_service_adjust_key_in_wpm(int delta)
{
    int next = (int)s_key_in_wpm + delta;
    if (next < (int)KEYER_MIN_TX_WPM) {
        next = (int)KEYER_MIN_TX_WPM;
    } else if (next > (int)KEYER_MAX_TX_WPM) {
        next = (int)KEYER_MAX_TX_WPM;
    }

    keyer_service_set_key_in_wpm((uint8_t)next);
}

uint8_t keyer_service_get_key_out_wpm(void)
{
    return s_key_out_wpm;
}

void keyer_service_set_key_out_wpm(uint8_t wpm)
{
    s_key_out_wpm = (uint8_t)keyer_clamp_tx_wpm(wpm);
    ESP_LOGI(TAG, "key out wpm: %u", (unsigned)s_key_out_wpm);
}

void keyer_service_adjust_key_out_wpm(int delta)
{
    int next = (int)s_key_out_wpm + delta;
    if (next < (int)KEYER_MIN_TX_WPM) {
        next = (int)KEYER_MIN_TX_WPM;
    } else if (next > (int)KEYER_MAX_TX_WPM) {
        next = (int)KEYER_MAX_TX_WPM;
    }

    keyer_service_set_key_out_wpm((uint8_t)next);
}

const char *keyer_service_io_mode_label(keyer_io_mode_t mode)
{
    switch (mode) {
    case KEYER_IO_PADDLE:
        return "Paddle";
    case KEYER_IO_PADDLE_R:
        return "PaddleR";
    case KEYER_IO_SK:
        return "SK";
    case KEYER_IO_SK_MONO:
        return "SK-Mono";
    default:
        return "Unknown";
    }
}

void keyer_service_set_input_mode(keyer_input_mode_t mode)
{
    switch (mode) {
    case KEYER_INPUT_STRAIGHT_KEY:
        keyer_service_set_key_in_mode(KEYER_IO_SK);
        break;
    case KEYER_INPUT_SINGLE_PADDLE:
    case KEYER_INPUT_DUAL_PADDLE:
        keyer_service_set_key_in_mode(KEYER_IO_PADDLE);
        break;
    default:
        ESP_LOGW(TAG, "unknown legacy input mode: %s", keyer_input_mode_name(mode));
        keyer_service_set_key_in_mode(KEYER_IO_PADDLE);
        break;
    }
}

uint16_t keyer_service_get_tx_wpm(void)
{
    /*
     * TX speed belongs to keyer_service. Step 3 exposes adjustment controls,
     * but paddle timing and full keyer logic still come later.
     */
    return s_key_in_wpm;
}

void keyer_service_set_tx_wpm(uint16_t wpm)
{
    keyer_service_set_key_in_wpm((uint8_t)keyer_clamp_tx_wpm(wpm));
}

void keyer_service_adjust_tx_wpm(int delta)
{
    keyer_service_adjust_key_in_wpm(delta);
}

void keyer_service_update(void)
{
    /*
     * Milestone 1 stub: debounce, paddle reads, and future iambic logic belong
     * here. No GPIO is touched until a board-specific keyer port is added.
     */
}

keyer_event_t keyer_service_poll_event(void)
{
    return KEYER_NO_EVENT;
}

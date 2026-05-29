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

static const keyer_event_t KEYER_NO_EVENT = {
    .type = KEYER_EVENT_NONE,
    .decoded_char = '\0',
    .duration_ms = 0,
};

static keyer_input_mode_t s_input_mode = KEYER_INPUT_STRAIGHT_KEY;
static uint16_t s_tx_wpm = KEYER_DEFAULT_TX_WPM;

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

void keyer_service_init(void)
{
    ESP_LOGI(TAG, "initialized paddle/key owner (Milestone 1 stub)");
    ESP_LOGI(TAG, "input mode: %s", keyer_input_mode_name(s_input_mode));
    ESP_LOGI(TAG, "tx wpm owner: %u", (unsigned)s_tx_wpm);
}

void keyer_service_set_input_mode(keyer_input_mode_t mode)
{
    s_input_mode = mode;
    ESP_LOGI(TAG, "input mode: %s", keyer_input_mode_name(s_input_mode));
}

uint16_t keyer_service_get_tx_wpm(void)
{
    /*
     * TX speed belongs to keyer_service. Step 3 exposes adjustment controls,
     * but paddle timing and full keyer logic still come later.
     */
    return s_tx_wpm;
}

void keyer_service_set_tx_wpm(uint16_t wpm)
{
    s_tx_wpm = keyer_clamp_tx_wpm(wpm);
    ESP_LOGI(TAG, "set tx wpm: %u", (unsigned)s_tx_wpm);
}

void keyer_service_adjust_tx_wpm(int delta)
{
    int next = (int)s_tx_wpm + delta;
    if (next < (int)KEYER_MIN_TX_WPM) {
        next = (int)KEYER_MIN_TX_WPM;
    }

    if (next > (int)KEYER_MAX_TX_WPM) {
        next = (int)KEYER_MAX_TX_WPM;
    }

    keyer_service_set_tx_wpm((uint16_t)next);
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

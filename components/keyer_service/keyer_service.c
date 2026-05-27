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

static const keyer_event_t KEYER_NO_EVENT = {
    .type = KEYER_EVENT_NONE,
    .decoded_char = '\0',
    .duration_ms = 0,
};

static keyer_input_mode_t s_input_mode = KEYER_INPUT_STRAIGHT_KEY;

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
}

void keyer_service_set_input_mode(keyer_input_mode_t mode)
{
    s_input_mode = mode;
    ESP_LOGI(TAG, "input mode: %s", keyer_input_mode_name(s_input_mode));
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

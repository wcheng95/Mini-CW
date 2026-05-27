/*
 * app_core
 *
 * Responsibility: Owns the Mini-CW application state machine, initializes
 * services, and routes high-level events.
 * Hardware ownership: none. app_core must not call GPIO, display, speaker,
 * SD/SPIFFS, RTC, or PMU APIs directly.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_MODE_TONE_TEST = 0,
    APP_MODE_RX_PRACTICE,
    APP_MODE_TX_PRACTICE,
    APP_MODE_CALLSIGN,
    APP_MODE_QSO,
    APP_MODE_STATS,
    APP_MODE_MENU,
} app_mode_t;

void app_core_init(void);
void app_core_run(void);

app_mode_t app_core_get_mode(void);
const char *app_core_mode_to_string(app_mode_t mode);

#ifdef __cplusplus
}
#endif

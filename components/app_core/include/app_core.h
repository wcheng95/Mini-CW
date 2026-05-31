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
    APP_MODE_PRACTICE = 0,
    APP_MODE_KEYER,
    APP_MODE_LESSONS,
    APP_MODE_SYSTEM,
} app_mode_t;

void app_core_init(void);
void app_core_run(void);

app_mode_t app_core_get_mode(void);
const char *app_core_mode_to_string(app_mode_t mode);

#ifdef __cplusplus
}
#endif

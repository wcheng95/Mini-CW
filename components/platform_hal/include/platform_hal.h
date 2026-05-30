/*
 * platform_hal
 *
 * Responsibility: Owns low-level board initialization and general platform
 * services.
 * Hardware ownership: RTC/time, battery/PMU, and board-level setup that does
 * not belong to a more specific service owner.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void platform_hal_init(void);
esp_err_t platform_hal_get_battery_percent(int *out_percent);
esp_err_t platform_hal_enter_deep_sleep(void);

#ifdef __cplusplus
}
#endif

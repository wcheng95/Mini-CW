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

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PLATFORM_HAL_TIME_SOURCE_SOFTWARE = 0,
    PLATFORM_HAL_TIME_SOURCE_DS3231,
    PLATFORM_HAL_TIME_SOURCE_GPS,
} platform_hal_time_source_t;

typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    platform_hal_time_source_t source;
} platform_hal_datetime_t;

void platform_hal_init(void);
esp_err_t platform_hal_get_datetime(platform_hal_datetime_t *out_datetime);
esp_err_t platform_hal_set_datetime(const platform_hal_datetime_t *datetime,
                                    platform_hal_time_source_t source);
platform_hal_time_source_t platform_hal_get_time_source(void);
bool platform_hal_ds3231_available(void);
esp_err_t platform_hal_get_battery_percent(int *out_percent);
esp_err_t platform_hal_enter_deep_sleep(void);

#ifdef __cplusplus
}
#endif

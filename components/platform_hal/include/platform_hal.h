/*
 * platform_hal
 *
 * Responsibility: Owns low-level board initialization and general platform
 * services.
 * Hardware ownership: RTC/time, battery/PMU, and board-level setup that does
 * not belong to a more specific service owner.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void platform_hal_init(void);

#ifdef __cplusplus
}
#endif

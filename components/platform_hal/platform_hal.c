/*
 * platform_hal
 *
 * Responsibility: Owns low-level board initialization and general platform
 * services.
 * Hardware ownership: RTC/time, battery/PMU, and shared platform setup.
 * Milestone 1 avoids board-specific driver calls until target details are set.
 */

#include "platform_hal.h"

#include "esp_log.h"

static const char *TAG = "platform_hal";

void platform_hal_init(void)
{
    ESP_LOGI(TAG, "initialized platform HAL (Milestone 1 stub)");
}

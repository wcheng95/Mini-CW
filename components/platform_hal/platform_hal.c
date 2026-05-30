/*
 * platform_hal
 *
 * Responsibility: Owns low-level board initialization and general platform
 * services.
 * Hardware ownership: RTC/time, battery/PMU, and shared platform setup.
 * Milestone 1 avoids board-specific driver calls until target details are set.
 */

#include "platform_hal.h"

#include "board_power.h"
#include "esp_log.h"
#include "esp_sleep.h"

#include "driver/gpio.h"

static const char *TAG = "platform_hal";

void platform_hal_init(void)
{
    ESP_LOGI(TAG, "initialized platform HAL");
}

esp_err_t platform_hal_get_battery_percent(int *out_percent)
{
    board_power_status_t status;
    esp_err_t err;
    int percent;

    if (out_percent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = board_power_read(&status);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "battery read failed: %s", esp_err_to_name(err));
        return err;
    }

    percent = status.percent;
    if (!status.valid || percent < 0 || percent > 100) {
        percent = 0;
    }

    *out_percent = percent;
    return ESP_OK;
}

esp_err_t platform_hal_enter_deep_sleep(void)
{
    esp_err_t err;

    ESP_LOGI(TAG, "entering deep sleep (GPIO0 wake)");

    err = esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to configure deep sleep wake: %s", esp_err_to_name(err));
        return err;
    }

    esp_deep_sleep_start();
    return ESP_OK;
}

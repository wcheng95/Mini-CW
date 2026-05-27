/*
 * storage_service
 *
 * Responsibility: Owns profile, lesson, and session log persistence.
 * Hardware ownership: SD/SPIFFS/file access. Milestone 1 logs persistence
 * requests instead of mounting storage or opening files.
 */

#include "storage_service.h"

#include "esp_log.h"

static const char *TAG = "storage_service";

void storage_service_init(void)
{
    ESP_LOGI(TAG, "initialized storage owner (Milestone 1 stub)");
}

bool storage_profile_load(void)
{
    ESP_LOGI(TAG, "load profile stub");
    return true;
}

bool storage_profile_save(void)
{
    ESP_LOGI(TAG, "save profile stub");
    return true;
}

bool storage_session_log_append(const char *line)
{
    ESP_LOGI(TAG, "append session log stub: %s", line ? line : "");
    return true;
}

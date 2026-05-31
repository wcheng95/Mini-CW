/*
 * storage_service
 *
 * Responsibility: Owns future profile, lesson, and session log persistence.
 * Hardware ownership: SD/SPIFFS/FATFS/file access. Persistence is deliberately
 * disabled for the Lessons trial build; settings/results stay in RAM until the
 * FATFS-backed setting/log model is ready.
 */

#include "storage_service.h"

#include "esp_log.h"

static const char *TAG = "storage_service";

void storage_service_init(void)
{
    ESP_LOGI(TAG, "initialized storage owner: persistence disabled for trial build");
}

bool storage_profile_load(void)
{
    ESP_LOGI(TAG, "profile load skipped: persistence disabled");
    return false;
}

bool storage_profile_save(void)
{
    ESP_LOGI(TAG, "profile save skipped: persistence disabled");
    return false;
}

bool storage_session_log_append(const char *line)
{
    ESP_LOGI(TAG, "session log skipped: %s", line ? line : "");
    return false;
}

bool storage_lesson_load(cw_lesson_config_t *config, cw_lesson_result_t *result)
{
    (void)config;
    (void)result;
    ESP_LOGI(TAG, "lesson load skipped: persistence disabled");
    return false;
}

bool storage_lesson_save_config(const cw_lesson_config_t *config)
{
    (void)config;
    ESP_LOGI(TAG, "lesson config save skipped: persistence disabled");
    return false;
}

bool storage_lesson_save_result(const cw_lesson_result_t *result)
{
    (void)result;
    ESP_LOGI(TAG, "lesson result save skipped: persistence disabled");
    return false;
}

bool storage_word_load(cw_word_config_t *config, cw_word_result_t *result)
{
    (void)config;
    (void)result;
    ESP_LOGI(TAG, "word load skipped: persistence disabled");
    return false;
}

bool storage_word_save_config(const cw_word_config_t *config)
{
    (void)config;
    ESP_LOGI(TAG, "word config save skipped: persistence disabled");
    return false;
}

bool storage_word_save_result(const cw_word_result_t *result)
{
    (void)result;
    ESP_LOGI(TAG, "word result save skipped: persistence disabled");
    return false;
}

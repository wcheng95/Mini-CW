/*
 * app_core
 *
 * Responsibility: Owns the Mini-CW application state machine, initializes
 * services, and routes high-level events between services.
 * Hardware ownership: none. app_core only talks to service/HAL APIs and must
 * not directly access board hardware.
 */

#include "app_core.h"

#include "audio_service.h"
#include "cw_trainer_service.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "keyer_service.h"
#include "platform_hal.h"
#include "storage_service.h"
#include "ui_service.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "app_core";

#define APP_INPUT_POLL_MS 5U
#define APP_SYSTEM_CONFIG_SAVE_DELAY_MS 1000U
#define APP_SYSTEM_TIME_REFRESH_MS 1000U
#define APP_SYSTEM_DEFAULT_DATE "2026-01-01"
#define APP_SYSTEM_DEFAULT_TIME "00:00:00"
#define APP_KEYER_LOG_TEXT_MAX 1024U
#define APP_KEYER_LOG_MINUTE_LEN 12U
#define APP_KEYER_LOG_TIMESTAMP_LEN 15U
#define APP_KEYER_LOG_LINE_MAX (APP_KEYER_LOG_TEXT_MAX + 40U)

static TickType_t app_core_ms_to_delay_ticks(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return ticks > 0 ? ticks : 1;
}

static bool app_core_tick_reached(TickType_t now, TickType_t due)
{
    return (TickType_t)(now - due) < (TickType_t)(UINT32_MAX / 2U);
}

typedef struct {
    app_mode_t mode;
    bool initialized;
} app_state_t;

static app_state_t s_app = {
    .mode = APP_MODE_KEYER,
    .initialized = false,
};
static bool s_system_config_dirty;
static TickType_t s_system_config_save_due;
static TickType_t s_system_time_refresh_due;
static bool s_keyer_config_dirty;
static TickType_t s_keyer_config_save_due;

typedef struct {
    bool tx_pending;
    TickType_t tx_due;

    bool m1_repeat_active;
    bool m1_repeat_waiting;
    TickType_t m1_repeat_due;
    bool last_append_was_message;
    uint32_t tx_revision;

    bool tune_active;
    bool tune_timeout_pending;
    TickType_t tune_timeout_due;
    bool tune_last_latched;
    bool tune_last_output_active;

    bool log_active;
    bool log_truncated;
    char log_minute[APP_KEYER_LOG_MINUTE_LEN + 1U];
    char log_timestamp[APP_KEYER_LOG_TIMESTAMP_LEN + 1U];
    char log_text[APP_KEYER_LOG_TEXT_MAX + 1U];
    size_t log_text_len;
} app_keyer_state_t;

static app_keyer_state_t s_keyer;

static app_mode_t app_core_ui_mode_to_app(ui_service_mode_t mode)
{
    switch (mode) {
    case UI_SERVICE_MODE_PLAINTEXT:
        return APP_MODE_PLAINTEXT;
    case UI_SERVICE_MODE_LESSONS:
        return APP_MODE_LESSONS;
    case UI_SERVICE_MODE_WORDS:
        return APP_MODE_WORDS;
    case UI_SERVICE_MODE_CALLSIGNS:
        return APP_MODE_CALLSIGNS;
    case UI_SERVICE_MODE_SYSTEM:
        return APP_MODE_SYSTEM;
    case UI_SERVICE_MODE_KEYER:
    default:
        return APP_MODE_KEYER;
    }
}

static void app_core_sync_mode_from_ui(void)
{
    s_app.mode = app_core_ui_mode_to_app(ui_service_get_mode());
    ESP_LOGI(TAG, "active mode: %s", app_core_mode_to_string(s_app.mode));
}

static bool app_core_is_leap_year(uint16_t year)
{
    return (year % 4U == 0U) && ((year % 100U) != 0U || (year % 400U) == 0U);
}

static uint8_t app_core_days_in_month(uint16_t year, uint8_t month)
{
    static const uint8_t days[] = {
        31U,
        28U,
        31U,
        30U,
        31U,
        30U,
        31U,
        31U,
        30U,
        31U,
        30U,
        31U,
    };

    if (month < 1U || month > 12U) {
        return 0U;
    }

    if (month == 2U && app_core_is_leap_year(year)) {
        return 29U;
    }

    return days[month - 1U];
}

static bool app_core_parse_fixed_uint(const char *text,
                                      size_t offset,
                                      size_t count,
                                      uint16_t *out_value)
{
    uint16_t value = 0U;

    if (text == NULL || out_value == NULL || count == 0U) {
        return false;
    }

    for (size_t i = 0U; i < count; ++i) {
        unsigned char ch = (unsigned char)text[offset + i];
        if (!isdigit(ch)) {
            return false;
        }
        value = (uint16_t)(value * 10U + (uint16_t)(ch - '0'));
    }

    *out_value = value;
    return true;
}

static bool app_core_parse_date_string(const char *date,
                                       uint16_t *out_year,
                                       uint8_t *out_month,
                                       uint8_t *out_day)
{
    uint16_t year;
    uint16_t month;
    uint16_t day;

    if (date == NULL || out_year == NULL || out_month == NULL || out_day == NULL ||
        strlen(date) != STORAGE_SYSTEM_DATE_LEN) {
        return false;
    }

    if (date[4] != '-' || date[7] != '-') {
        return false;
    }

    if (!app_core_parse_fixed_uint(date, 0U, 4U, &year) ||
        !app_core_parse_fixed_uint(date, 5U, 2U, &month) ||
        !app_core_parse_fixed_uint(date, 8U, 2U, &day)) {
        return false;
    }

    if (year < 2024U || year > 2099U || month < 1U || month > 12U || day < 1U ||
        day > app_core_days_in_month(year, (uint8_t)month)) {
        return false;
    }

    *out_year = year;
    *out_month = (uint8_t)month;
    *out_day = (uint8_t)day;
    return true;
}

static bool app_core_parse_time_string(const char *time,
                                       uint8_t *out_hour,
                                       uint8_t *out_minute,
                                       uint8_t *out_second)
{
    uint16_t hour;
    uint16_t minute;
    uint16_t second;

    if (time == NULL || out_hour == NULL || out_minute == NULL || out_second == NULL ||
        strlen(time) != STORAGE_SYSTEM_TIME_LEN) {
        return false;
    }

    if (time[2] != ':' || time[5] != ':') {
        return false;
    }

    if (!app_core_parse_fixed_uint(time, 0U, 2U, &hour) ||
        !app_core_parse_fixed_uint(time, 3U, 2U, &minute) ||
        !app_core_parse_fixed_uint(time, 6U, 2U, &second)) {
        return false;
    }

    if (hour > 23U || minute > 59U || second > 59U) {
        return false;
    }

    *out_hour = (uint8_t)hour;
    *out_minute = (uint8_t)minute;
    *out_second = (uint8_t)second;
    return true;
}

static bool app_core_datetime_from_strings(const char *date,
                                           const char *time,
                                           platform_hal_datetime_t *out_datetime)
{
    platform_hal_datetime_t datetime = {
        .source = PLATFORM_HAL_TIME_SOURCE_SOFTWARE,
    };

    if (out_datetime == NULL) {
        return false;
    }

    if (!app_core_parse_date_string(date, &datetime.year, &datetime.month, &datetime.day) ||
        !app_core_parse_time_string(time, &datetime.hour, &datetime.minute, &datetime.second)) {
        return false;
    }

    *out_datetime = datetime;
    return true;
}

static void app_core_format_datetime_date(const platform_hal_datetime_t *datetime,
                                          char *dest,
                                          size_t dest_size)
{
    if (dest == NULL || dest_size == 0U) {
        return;
    }

    if (datetime == NULL) {
        snprintf(dest, dest_size, "%s", APP_SYSTEM_DEFAULT_DATE);
        return;
    }

    snprintf(dest,
             dest_size,
             "%04u-%02u-%02u",
             (unsigned)datetime->year,
             (unsigned)datetime->month,
             (unsigned)datetime->day);
}

static void app_core_format_datetime_time(const platform_hal_datetime_t *datetime,
                                          char *dest,
                                          size_t dest_size)
{
    if (dest == NULL || dest_size == 0U) {
        return;
    }

    if (datetime == NULL) {
        snprintf(dest, dest_size, "%s", APP_SYSTEM_DEFAULT_TIME);
        return;
    }

    snprintf(dest,
             dest_size,
             "%02u:%02u:%02u",
             (unsigned)datetime->hour,
             (unsigned)datetime->minute,
             (unsigned)datetime->second);
}

static storage_system_config_t app_core_current_system_config(void)
{
    platform_hal_datetime_t datetime;
    storage_system_config_t config = {
        .volume = audio_service_get_volume(),
        .tone_hz = audio_service_get_tone_hz(),
        .key_in_mode = keyer_service_get_key_in_mode(),
        .key_in_wpm = keyer_service_get_key_in_wpm(),
    };

    if (platform_hal_get_datetime(&datetime) == ESP_OK) {
        app_core_format_datetime_date(&datetime, config.date, sizeof(config.date));
        app_core_format_datetime_time(&datetime, config.time, sizeof(config.time));
    } else {
        snprintf(config.date, sizeof(config.date), "%s", APP_SYSTEM_DEFAULT_DATE);
        snprintf(config.time, sizeof(config.time), "%s", APP_SYSTEM_DEFAULT_TIME);
    }

    return config;
}

static void app_core_keyer_log_reset(void)
{
    s_keyer.log_active = false;
    s_keyer.log_truncated = false;
    s_keyer.log_minute[0] = '\0';
    s_keyer.log_timestamp[0] = '\0';
    s_keyer.log_text[0] = '\0';
    s_keyer.log_text_len = 0U;
}

static bool app_core_keyer_log_format_time(char *timestamp,
                                           size_t timestamp_size,
                                           char *minute,
                                           size_t minute_size)
{
    platform_hal_datetime_t datetime = {
        .year = 2026U,
        .month = 1U,
        .day = 1U,
        .hour = 0U,
        .minute = 0U,
        .second = 0U,
        .source = PLATFORM_HAL_TIME_SOURCE_SOFTWARE,
    };
    int timestamp_len;
    int minute_len;

    if (timestamp == NULL || timestamp_size == 0U || minute == NULL || minute_size == 0U) {
        return false;
    }

    (void)platform_hal_get_datetime(&datetime);
    timestamp_len = snprintf(timestamp,
                             timestamp_size,
                             "%04u%02u%02u %02u%02u%02u",
                             (unsigned)datetime.year,
                             (unsigned)datetime.month,
                             (unsigned)datetime.day,
                             (unsigned)datetime.hour,
                             (unsigned)datetime.minute,
                             (unsigned)datetime.second);
    minute_len = snprintf(minute,
                          minute_size,
                          "%04u%02u%02u%02u%02u",
                          (unsigned)datetime.year,
                          (unsigned)datetime.month,
                          (unsigned)datetime.day,
                          (unsigned)datetime.hour,
                          (unsigned)datetime.minute);

    return timestamp_len > 0 && (size_t)timestamp_len < timestamp_size &&
           minute_len > 0 && (size_t)minute_len < minute_size;
}

static bool app_core_keyer_log_format_line(char *line, size_t line_size)
{
    const char *trunc_suffix = s_keyer.log_truncated ? " [TRUNC]" : "";
    int line_len;

    if (line == NULL || line_size == 0U) {
        return false;
    }

    line_len = snprintf(line,
                        line_size,
                        "T [%s][x.xxx] %s%s",
                        s_keyer.log_timestamp,
                        s_keyer.log_text,
                        trunc_suffix);
    if (line_len <= 0 || (size_t)line_len >= line_size) {
        ESP_LOGW(TAG, "keyer log line format failed or overflowed");
        return false;
    }

    return true;
}

static bool app_core_keyer_log_flush(void)
{
    char line[APP_KEYER_LOG_LINE_MAX];
    bool ok = true;

    if (!s_keyer.log_active) {
        return true;
    }

    if (s_keyer.log_text_len > 0U) {
        if (!app_core_keyer_log_format_line(line, sizeof(line))) {
            ESP_LOGW(TAG, "keyer log line dropped after format failure");
            ok = false;
        } else if (!storage_session_log_append(line)) {
            ESP_LOGW(TAG, "keyer log append failed; line dropped");
            ok = false;
        }
    }

    app_core_keyer_log_reset();
    return ok;
}

static bool app_core_keyer_log_flush_if_minute_changed(void)
{
    char timestamp[APP_KEYER_LOG_TIMESTAMP_LEN + 1U];
    char minute[APP_KEYER_LOG_MINUTE_LEN + 1U];

    if (!s_keyer.log_active) {
        return true;
    }

    if (!app_core_keyer_log_format_time(timestamp, sizeof(timestamp), minute, sizeof(minute))) {
        return false;
    }

    if (strcmp(minute, s_keyer.log_minute) != 0) {
        (void)app_core_keyer_log_flush();
    }

    return true;
}

static bool app_core_keyer_log_begin(void)
{
    if (!app_core_keyer_log_format_time(s_keyer.log_timestamp,
                                        sizeof(s_keyer.log_timestamp),
                                        s_keyer.log_minute,
                                        sizeof(s_keyer.log_minute))) {
        return false;
    }

    s_keyer.log_active = true;
    s_keyer.log_truncated = false;
    s_keyer.log_text[0] = '\0';
    s_keyer.log_text_len = 0U;
    return true;
}

static bool app_core_keyer_log_normalize_char(char ch, char *out_ch)
{
    unsigned char value = (unsigned char)ch;

    if (out_ch == NULL) {
        return false;
    }

    if (ch == '\r' || ch == '\n') {
        *out_ch = ' ';
        return true;
    }

    if (!isprint(value)) {
        return false;
    }

    *out_ch = ch;
    return true;
}

static void app_core_keyer_log_append_char(char ch)
{
    char log_ch;

    if (!app_core_keyer_log_normalize_char(ch, &log_ch)) {
        return;
    }

    if (!app_core_keyer_log_flush_if_minute_changed()) {
        return;
    }
    if (!s_keyer.log_active && !app_core_keyer_log_begin()) {
        return;
    }

    if (s_keyer.log_text_len >= APP_KEYER_LOG_TEXT_MAX) {
        if (!s_keyer.log_truncated) {
            s_keyer.log_truncated = true;
            ESP_LOGW(TAG, "keyer log minute text truncated");
        }
        return;
    }

    s_keyer.log_text[s_keyer.log_text_len++] = log_ch;
    s_keyer.log_text[s_keyer.log_text_len] = '\0';
}

static void app_core_keyer_log_append_text(const char *text)
{
    if (text == NULL) {
        return;
    }

    while (*text != '\0') {
        app_core_keyer_log_append_char(*text++);
    }
}

static bool app_core_keyer_log_tx_will_insert_space(const char *text, bool insert_space)
{
    char tail[2];

    if (!insert_space || text == NULL || text[0] == '\0' || text[0] == ' ') {
        return false;
    }

    keyer_service_tx_copy_text(tail, sizeof(tail));
    return tail[0] != '\0' && tail[0] != ' ';
}

static void app_core_keyer_log_backspace(void)
{
    if (!app_core_keyer_log_flush_if_minute_changed()) {
        return;
    }

    if (!s_keyer.log_active || s_keyer.log_text_len == 0U) {
        return;
    }

    --s_keyer.log_text_len;
    s_keyer.log_text[s_keyer.log_text_len] = '\0';
    if (s_keyer.log_text_len == 0U) {
        app_core_keyer_log_reset();
    }
}

static bool app_core_system_config_equal_common(const storage_system_config_t *a,
                                                const storage_system_config_t *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }

    return a->volume == b->volume && a->tone_hz == b->tone_hz &&
           a->key_in_mode == b->key_in_mode && a->key_in_wpm == b->key_in_wpm;
}

static bool app_core_system_config_equal(const storage_system_config_t *a,
                                         const storage_system_config_t *b)
{
    return app_core_system_config_equal_common(a, b) &&
           strcmp(a->date, b->date) == 0 && strcmp(a->time, b->time) == 0;
}

static bool app_core_system_config_equal_ignore_datetime(const storage_system_config_t *a,
                                                        const storage_system_config_t *b)
{
    return app_core_system_config_equal_common(a, b);
}

static void app_core_mark_system_config_dirty(void)
{
    s_system_config_dirty = true;
    s_system_config_save_due =
        xTaskGetTickCount() + app_core_ms_to_delay_ticks(APP_SYSTEM_CONFIG_SAVE_DELAY_MS);
}

static void app_core_mark_keyer_config_dirty(void)
{
    s_keyer_config_dirty = true;
    s_keyer_config_save_due =
        xTaskGetTickCount() + app_core_ms_to_delay_ticks(APP_SYSTEM_CONFIG_SAVE_DELAY_MS);
}

static void app_core_maybe_refresh_system_time(void)
{
    TickType_t now = xTaskGetTickCount();

    if (s_app.mode != APP_MODE_SYSTEM) {
        s_system_time_refresh_due =
            now + app_core_ms_to_delay_ticks(APP_SYSTEM_TIME_REFRESH_MS);
        return;
    }

    if (!app_core_tick_reached(now, s_system_time_refresh_due)) {
        return;
    }

    s_system_time_refresh_due = now + app_core_ms_to_delay_ticks(APP_SYSTEM_TIME_REFRESH_MS);
    ui_service_refresh();
}

static void app_core_maybe_save_dirty_config(void)
{
    TickType_t now;
    storage_system_config_t config;
    keyer_config_t keyer_config;

    if (!s_system_config_dirty && !s_keyer_config_dirty) {
        return;
    }

    now = xTaskGetTickCount();

    if (audio_service_is_busy() || keyer_service_is_tx_active()) {
        if (s_system_config_dirty) {
            s_system_config_save_due =
                now + app_core_ms_to_delay_ticks(APP_SYSTEM_CONFIG_SAVE_DELAY_MS);
        }
        if (s_keyer_config_dirty) {
            s_keyer_config_save_due =
                now + app_core_ms_to_delay_ticks(APP_SYSTEM_CONFIG_SAVE_DELAY_MS);
        }
        return;
    }

    if (s_system_config_dirty && app_core_tick_reached(now, s_system_config_save_due)) {
        config = app_core_current_system_config();
        if (storage_system_save_config(&config)) {
            s_system_config_dirty = false;
        } else {
            s_system_config_save_due =
                now + app_core_ms_to_delay_ticks(APP_SYSTEM_CONFIG_SAVE_DELAY_MS);
        }
    }

    if (s_keyer_config_dirty && app_core_tick_reached(now, s_keyer_config_save_due)) {
        keyer_service_get_config_copy(&keyer_config);
        if (storage_keyer_save_config(&keyer_config)) {
            s_keyer_config_dirty = false;
        } else {
            s_keyer_config_save_due =
                now + app_core_ms_to_delay_ticks(APP_SYSTEM_CONFIG_SAVE_DELAY_MS);
        }
    }
}

static void app_core_apply_system_config(const storage_system_config_t *config, bool apply_datetime)
{
    platform_hal_datetime_t datetime;

    if (config == NULL) {
        return;
    }

    audio_service_set_volume(config->volume);
    audio_service_set_tone_hz(config->tone_hz);
    keyer_service_set_key_in_mode(config->key_in_mode);
    keyer_service_set_key_in_wpm(config->key_in_wpm);

    if (apply_datetime &&
        app_core_datetime_from_strings(config->date, config->time, &datetime)) {
        (void)platform_hal_set_datetime(&datetime, PLATFORM_HAL_TIME_SOURCE_SOFTWARE);
    }
}

static void app_core_reload_qsocalls(void)
{
    keyer_op_entry_t *entries = NULL;
    size_t count = 0U;

    if (storage_qsocalls_load(&entries, &count)) {
        keyer_service_set_op_table(entries, count);
    }
}

static void app_core_load_persisted_settings(void)
{
    storage_system_config_t system_config;
    keyer_config_t keyer_config;
    cw_lesson_config_t lesson_config;
    cw_lesson_result_t lesson_result = {0};
    cw_word_config_t word_config;
    cw_word_result_t word_result = {0};
    cw_callsign_config_t callsign_config;
    cw_callsign_result_t callsign_result = {0};
    cw_plaintext_config_t plaintext_config;
    cw_plaintext_result_t plaintext_result = {0};

    if (!storage_profile_load()) {
        return;
    }

    system_config = app_core_current_system_config();
    if (storage_system_load_config(&system_config)) {
        bool ds3231_time_active =
            platform_hal_get_time_source() == PLATFORM_HAL_TIME_SOURCE_DS3231;
        bool apply_datetime = !ds3231_time_active;
        app_core_apply_system_config(&system_config, apply_datetime);
        storage_system_config_t applied_config = app_core_current_system_config();
        bool config_equal = ds3231_time_active
                                ? app_core_system_config_equal_ignore_datetime(&system_config,
                                                                              &applied_config)
                                : app_core_system_config_equal(&system_config, &applied_config);
        if (!config_equal) {
            if (ds3231_time_active) {
                snprintf(applied_config.date, sizeof(applied_config.date), "%s", system_config.date);
                snprintf(applied_config.time, sizeof(applied_config.time), "%s", system_config.time);
            }
            storage_system_save_config(&applied_config);
        }
    }

    keyer_config = *keyer_service_get_config();
    if (storage_keyer_load_config(&keyer_config)) {
        keyer_service_set_config(&keyer_config);
    }

    lesson_config = *cw_trainer_lesson_get_config();
    if (storage_lesson_load(&lesson_config, &lesson_result)) {
        cw_trainer_lesson_load_persisted(&lesson_config, &lesson_result);
    }

    word_config = *cw_trainer_word_get_config();
    if (storage_word_load(&word_config, &word_result)) {
        cw_trainer_word_load_persisted(&word_config, &word_result);
    }

    callsign_config = *cw_trainer_callsign_get_config();
    if (storage_callsign_load(&callsign_config, &callsign_result)) {
        cw_trainer_callsign_load_persisted(&callsign_config, &callsign_result);
    }

    plaintext_config = *cw_trainer_plaintext_get_config();
    if (storage_plaintext_load(&plaintext_config, &plaintext_result)) {
        cw_trainer_plaintext_load_persisted(&plaintext_config, &plaintext_result);
    }

    app_core_reload_qsocalls();
}

static void app_core_handle_lesson_select(void)
{
    const cw_lesson_view_t *view = cw_trainer_lesson_get_view();

    if (view != NULL && view->state == CW_LESSON_STATE_COPYING) {
        const cw_lesson_result_t *result = cw_trainer_lesson_submit();
        storage_lesson_save_result(result);
    } else {
        cw_trainer_lesson_start();
    }

    ui_service_refresh();
}

static void app_core_handle_word_select(void)
{
    const cw_word_view_t *view = cw_trainer_word_get_view();

    if (view != NULL && view->state == CW_WORD_STATE_COPYING) {
        const cw_word_result_t *result = cw_trainer_word_submit();
        const cw_word_view_t *updated_view = cw_trainer_word_get_view();
        if (updated_view != NULL && updated_view->state == CW_WORD_STATE_RESULT) {
            storage_word_save_result(result);
        }
    } else {
        cw_trainer_word_start();
    }

    ui_service_refresh();
}

static void app_core_handle_callsign_select(void)
{
    const cw_callsign_view_t *view = cw_trainer_callsign_get_view();

    if (view != NULL && view->state == CW_CALLSIGN_STATE_COPYING) {
        const cw_callsign_result_t *result = cw_trainer_callsign_submit();
        const cw_callsign_view_t *updated_view = cw_trainer_callsign_get_view();
        if (updated_view != NULL && updated_view->state == CW_CALLSIGN_STATE_RESULT) {
            storage_callsign_save_result(result);
        }
    } else {
        cw_trainer_callsign_start();
    }

    ui_service_refresh();
}

static void app_core_handle_plaintext_select(void)
{
    const cw_plaintext_view_t *view = cw_trainer_plaintext_get_view();

    if (view != NULL && view->state == CW_PLAINTEXT_STATE_COPYING) {
        const cw_plaintext_result_t *result = cw_trainer_plaintext_submit();
        storage_plaintext_save_result(result);
    } else {
        cw_trainer_plaintext_start();
    }

    ui_service_refresh();
}

static void app_core_keyer_set_tx_display(void)
{
    char tx_text[UI_INPUT_EVENT_TEXT_MAX + 1U];

    keyer_service_tx_copy_text(tx_text, sizeof(tx_text));
    ui_service_keyer_set_tx_text(tx_text);
}

static void app_core_keyer_sync_tx_display(bool force)
{
    uint32_t revision = keyer_service_tx_revision();

    if (force || revision != s_keyer.tx_revision) {
        s_keyer.tx_revision = revision;
        app_core_keyer_set_tx_display();
        ui_service_refresh();
    }
}

static void app_core_keyer_cancel_repeat(void)
{
    s_keyer.m1_repeat_active = false;
    s_keyer.m1_repeat_waiting = false;
}

static void app_core_keyer_clear_tx_fifo(void)
{
    s_keyer.tx_pending = false;
    s_keyer.last_append_was_message = false;
    app_core_keyer_cancel_repeat();
    keyer_service_tx_clear();
    app_core_keyer_sync_tx_display(true);
}

static void app_core_keyer_schedule_tx(void)
{
    uint8_t delay_s;

    if (keyer_service_is_tx_active()) {
        s_keyer.tx_pending = false;
        return;
    }

    delay_s = keyer_service_get_tx_delay_s();
    if (delay_s == 0U) {
        s_keyer.tx_pending = false;
        keyer_service_tx_start();
        app_core_keyer_sync_tx_display(false);
        return;
    }

    s_keyer.tx_pending = true;
    s_keyer.tx_due = xTaskGetTickCount() + app_core_ms_to_delay_ticks((uint32_t)delay_s * 1000U);
}

static void app_core_keyer_start_tx_now(void)
{
    if (!keyer_service_tx_has_text()) {
        s_keyer.tx_pending = false;
        return;
    }

    s_keyer.tx_pending = false;
    keyer_service_tx_start();
    app_core_keyer_sync_tx_display(false);
}

static void app_core_keyer_append_tx_char(char key)
{
    char normalized = key == ' ' ? ' ' : (char)toupper((unsigned char)key);
    char text[2] = {normalized, '\0'};
    bool insert_space;
    bool log_insert_space;

    if (normalized != ' ' && audio_service_get_cw_pattern(normalized) == NULL) {
        ui_service_keyer_set_status("Unsupported");
        return;
    }

    app_core_keyer_cancel_repeat();
    insert_space = s_keyer.last_append_was_message && normalized != ' ';
    log_insert_space = app_core_keyer_log_tx_will_insert_space(text, insert_space);
    if (!keyer_service_tx_append_text(text, insert_space)) {
        ui_service_keyer_set_status("TX buffer full");
        return;
    }

    if (log_insert_space) {
        app_core_keyer_log_append_char(' ');
    }
    app_core_keyer_log_append_text(text);
    if (insert_space) {
        keyer_service_op_feed_char(' ');
    }
    keyer_service_op_feed_char(normalized);
    s_keyer.last_append_was_message = false;
    app_core_keyer_sync_tx_display(true);
    app_core_keyer_schedule_tx();
}

static void app_core_keyer_backspace_tx(void)
{
    if (keyer_service_tx_backspace()) {
        app_core_keyer_log_backspace();
        if (!keyer_service_tx_has_text()) {
            s_keyer.tx_pending = false;
            s_keyer.last_append_was_message = false;
        }
        app_core_keyer_sync_tx_display(true);
    }
}

static void app_core_keyer_append_message(uint8_t message_index)
{
    const char *message;
    bool insert_space;
    bool log_insert_space;
    bool repeat_m1;

    if (message_index < 1U || message_index > KEYER_MESSAGE_COUNT) {
        return;
    }

    repeat_m1 = message_index == 1U;
    if (!repeat_m1) {
        app_core_keyer_cancel_repeat();
    }

    insert_space = keyer_service_tx_has_text();
    message = keyer_service_get_message((uint8_t)(message_index - 1U));
    log_insert_space = app_core_keyer_log_tx_will_insert_space(message, insert_space);
    if (!keyer_service_tx_append_text(message, insert_space)) {
        ui_service_keyer_set_status("TX buffer full");
        return;
    }

    if (log_insert_space) {
        app_core_keyer_log_append_char(' ');
    }
    app_core_keyer_log_append_text(message);
    if (insert_space) {
        keyer_service_op_feed_char(' ');
    }
    keyer_service_op_feed_text(message);
    if (repeat_m1) {
        s_keyer.m1_repeat_active = true;
        s_keyer.m1_repeat_waiting = false;
    }
    s_keyer.last_append_was_message = true;
    app_core_keyer_sync_tx_display(true);
    app_core_keyer_schedule_tx();
}

static void app_core_keyer_repeat_update(void)
{
    const char *message;

    if (!s_keyer.m1_repeat_active || s_keyer.tx_pending ||
        keyer_service_is_tx_active() || keyer_service_tx_has_text()) {
        if (keyer_service_is_tx_active() || keyer_service_tx_has_text() || s_keyer.tx_pending) {
            s_keyer.m1_repeat_waiting = false;
        }
        return;
    }

    if (!s_keyer.m1_repeat_waiting) {
        s_keyer.m1_repeat_waiting = true;
        s_keyer.m1_repeat_due =
            xTaskGetTickCount() +
            app_core_ms_to_delay_ticks((uint32_t)keyer_service_get_repeat_interval_s() * 1000U);
        return;
    }

    if (!app_core_tick_reached(xTaskGetTickCount(), s_keyer.m1_repeat_due)) {
        return;
    }

    s_keyer.m1_repeat_waiting = false;
    message = keyer_service_get_message(0U);
    if (!keyer_service_tx_append_text(message, false)) {
        ui_service_keyer_set_status("TX buffer full");
        return;
    }

    app_core_keyer_log_append_text(message);
    keyer_service_op_feed_text(message);
    s_keyer.last_append_was_message = true;
    app_core_keyer_sync_tx_display(true);
    app_core_keyer_start_tx_now();
}

static void app_core_keyer_sync_tune_ui(bool force)
{
    bool latched = s_keyer.tune_active && keyer_service_get_tune_latched();
    bool output_active = s_keyer.tune_active && keyer_service_get_tune_output_active();

    if (force || s_keyer.tune_last_latched != latched ||
        s_keyer.tune_last_output_active != output_active) {
        s_keyer.tune_last_latched = latched;
        s_keyer.tune_last_output_active = output_active;
        ui_service_refresh();
    }
}

static void app_core_keyer_set_tune_active(bool active)
{
    if (active) {
        app_core_keyer_clear_tx_fifo();
        s_keyer.tune_active = true;
        s_keyer.tune_timeout_pending = false;
        keyer_service_set_tune_active(true);
        ui_service_keyer_set_tune_active(true);
        app_core_keyer_sync_tune_ui(true);
        return;
    }

    s_keyer.tune_timeout_pending = false;
    keyer_service_set_tune_latched(false);
    keyer_service_set_tune_active(false);
    s_keyer.tune_active = false;
    ui_service_keyer_set_tune_active(false);
    app_core_keyer_sync_tune_ui(true);
}

static void app_core_keyer_set_tune_latched(bool latched)
{
    uint8_t timeout_s;

    if (!s_keyer.tune_active) {
        return;
    }

    keyer_service_set_tune_latched(latched);
    if (!latched) {
        s_keyer.tune_timeout_pending = false;
        app_core_keyer_sync_tune_ui(true);
        return;
    }

    timeout_s = keyer_service_get_tune_timeout_s();
    if (timeout_s == 0U) {
        s_keyer.tune_timeout_pending = false;
    } else {
        s_keyer.tune_timeout_pending = true;
        s_keyer.tune_timeout_due =
            xTaskGetTickCount() + app_core_ms_to_delay_ticks((uint32_t)timeout_s * 1000U);
    }
    app_core_keyer_sync_tune_ui(true);
}

static void app_core_keyer_update(void)
{
    if (keyer_service_take_sk_wpm_save_request()) {
        app_core_mark_keyer_config_dirty();
        ui_service_refresh();
    }

    if (s_keyer.tune_active) {
        if (s_keyer.tune_timeout_pending && !keyer_service_get_tune_latched()) {
            s_keyer.tune_timeout_pending = false;
        }

        if (s_keyer.tune_timeout_pending &&
            app_core_tick_reached(xTaskGetTickCount(), s_keyer.tune_timeout_due)) {
            keyer_service_set_tune_latched(false);
            s_keyer.tune_timeout_pending = false;
        }

        app_core_keyer_sync_tune_ui(false);
        return;
    }

    if (s_keyer.tx_pending && app_core_tick_reached(xTaskGetTickCount(), s_keyer.tx_due)) {
        app_core_keyer_start_tx_now();
    }

    app_core_keyer_repeat_update();
    app_core_keyer_sync_tx_display(false);
}

static void app_core_handle_volume_changed(const ui_input_event_t *event)
{
    if (event == NULL) {
        return;
    }

    audio_service_set_volume((uint8_t)event->value);
    audio_service_play_feedback_beep();
    app_core_mark_system_config_dirty();
    ui_service_refresh();
}

static void app_core_handle_tone_changed(const ui_input_event_t *event)
{
    if (event == NULL) {
        return;
    }

    audio_service_set_tone_hz((uint16_t)event->value);
    audio_service_play_feedback_beep();
    app_core_mark_system_config_dirty();
    ui_service_refresh();
}

static void app_core_handle_key_in_wpm_changed(const ui_input_event_t *event)
{
    if (event == NULL) {
        return;
    }

    keyer_service_set_key_in_wpm((uint8_t)event->value);
    app_core_mark_system_config_dirty();
    ui_service_refresh();
}

static void app_core_handle_key_in_mode_changed(const ui_input_event_t *event)
{
    int direction = 1;

    if (event != NULL && event->delta != 0) {
        direction = event->delta;
    }

    keyer_service_cycle_key_in_mode(direction);
    app_core_mark_system_config_dirty();
    ui_service_refresh();
}

static void app_core_handle_datetime_changed(const ui_input_event_t *event)
{
    platform_hal_datetime_t datetime;

    if (event == NULL || event->text[0] == '\0') {
        return;
    }

    if (platform_hal_get_datetime(&datetime) != ESP_OK) {
        ESP_LOGW(TAG, "date/time edit ignored: current RTC unavailable");
        ui_service_refresh();
        return;
    }

    if (event->setting == UI_SETTING_SYSTEM_DATE) {
        if (!app_core_parse_date_string(event->text,
                                        &datetime.year,
                                        &datetime.month,
                                        &datetime.day)) {
            ESP_LOGW(TAG, "date edit rejected: %s", event->text);
            ui_service_refresh();
            return;
        }
    } else if (event->setting == UI_SETTING_SYSTEM_TIME) {
        if (!app_core_parse_time_string(event->text,
                                        &datetime.hour,
                                        &datetime.minute,
                                        &datetime.second)) {
            ESP_LOGW(TAG, "time edit rejected: %s", event->text);
            ui_service_refresh();
            return;
        }
    } else {
        return;
    }

    if (platform_hal_set_datetime(&datetime, PLATFORM_HAL_TIME_SOURCE_SOFTWARE) != ESP_OK) {
        ESP_LOGW(TAG, "date/time edit rejected by platform HAL");
        ui_service_refresh();
        return;
    }

    app_core_mark_system_config_dirty();
    ui_service_refresh();
}

static void app_core_handle_key_out_mode_changed(const ui_input_event_t *event)
{
    int direction = 1;

    if (event != NULL && event->delta != 0) {
        direction = event->delta;
    }

    keyer_service_cycle_key_out_mode(direction);
    app_core_mark_keyer_config_dirty();
    ui_service_refresh();
}

static void app_core_handle_keyer_paddle_mode_changed(const ui_input_event_t *event)
{
    int direction = 1;

    if (event != NULL && event->delta != 0) {
        direction = event->delta;
    }

    keyer_service_cycle_paddle_mode(direction);
    app_core_mark_keyer_config_dirty();
    ui_service_refresh();
}

static void app_core_handle_keyer_mute_changed(const ui_input_event_t *event)
{
    if (event != NULL && event->setting == UI_SETTING_KEYER_MUTE) {
        keyer_service_set_mute(event->value != 0);
    } else {
        keyer_service_toggle_mute();
    }
    ui_service_refresh();
}

static void app_core_handle_keyer_config_changed(const ui_input_event_t *event)
{
    keyer_config_t config;

    if (event == NULL) {
        return;
    }

    keyer_service_get_config_copy(&config);
    switch (event->setting) {
    case UI_SETTING_KEYER_TX_DELAY_S:
        config.tx_delay_s = (uint8_t)event->value;
        break;
    case UI_SETTING_KEYER_TUNE_TIMEOUT_S:
        config.tune_timeout_s = (uint8_t)event->value;
        break;
    case UI_SETTING_KEYER_REPEAT_INTERVAL_S:
        config.repeat_interval_s = (uint8_t)event->value;
        break;
    case UI_SETTING_KEYER_SK_WPM:
        config.sk_wpm = (uint8_t)event->value;
        break;
    case UI_SETTING_KEYER_MYCALL:
        snprintf(config.mycall,
                 sizeof(config.mycall),
                 "%.*s",
                 (int)KEYER_MYCALL_MAX_LEN,
                 event->text);
        break;
    case UI_SETTING_KEYER_MESSAGE_1:
    case UI_SETTING_KEYER_MESSAGE_2:
    case UI_SETTING_KEYER_MESSAGE_3:
    case UI_SETTING_KEYER_MESSAGE_4:
    case UI_SETTING_KEYER_MESSAGE_5: {
        uint8_t index = (uint8_t)(event->setting - UI_SETTING_KEYER_MESSAGE_1);
        snprintf(config.message[index],
                 sizeof(config.message[index]),
                 "%.*s",
                 (int)KEYER_MESSAGE_MAX_LEN,
                 event->text);
        break;
    }
    case UI_SETTING_NONE:
    case UI_SETTING_VOLUME:
    case UI_SETTING_TONE_HZ:
    case UI_SETTING_KEY_IN_WPM:
    case UI_SETTING_KEY_IN_MODE:
    case UI_SETTING_LESSON:
    case UI_SETTING_LESSON_DURATION:
    case UI_SETTING_LESSON_CODE_WPM:
    case UI_SETTING_LESSON_EFFECTIVE_WPM:
    case UI_SETTING_LESSON_GROUP_LEN:
    case UI_SETTING_WORD_SPEED:
    case UI_SETTING_WORD_MIN_CHAR_WPM:
    case UI_SETTING_WORD_LESSON:
    case UI_SETTING_WORD_MAX_LEN:
    case UI_SETTING_WORD_MAX_WPM:
    case UI_SETTING_WORD_DELAY_S:
    case UI_SETTING_CALLSIGN_SPEED:
    case UI_SETTING_CALLSIGN_MIN_CHAR_WPM:
    case UI_SETTING_CALLSIGN_MAX_WPM:
    case UI_SETTING_CALLSIGN_DELAY_S:
    case UI_SETTING_PLAINTEXT_CODE_WPM:
    case UI_SETTING_PLAINTEXT_EFFECTIVE_WPM:
    case UI_SETTING_USB_DRIVE:
    case UI_SETTING_KEY_OUT_MODE:
    case UI_SETTING_KEYER_PADDLE_MODE:
    case UI_SETTING_KEYER_MUTE:
    default:
        break;
    }

    keyer_service_set_config(&config);
    app_core_mark_keyer_config_dirty();
    ui_service_refresh();
}

static void app_core_handle_usb_drive_changed(const ui_input_event_t *event)
{
    bool enabled = !storage_usb_drive_is_enabled();

    if (event != NULL && event->setting == UI_SETTING_USB_DRIVE) {
        enabled = event->value != 0;
    }

    if (enabled) {
        if (!app_core_keyer_log_flush()) {
            ESP_LOGW(TAG, "keyer log flush failed before USB Drive");
        }
    }

    /* Storage owns FATFS and USB MSC; app_core only routes the UI request. */
    if (storage_usb_drive_set_enabled(enabled) && !enabled) {
        app_core_load_persisted_settings();
    }
    ui_service_refresh();
}

static void app_core_handle_lesson_config_changed(const ui_input_event_t *event)
{
    cw_lesson_config_t config = *cw_trainer_lesson_get_config();

    if (event != NULL) {
        switch (event->setting) {
        case UI_SETTING_LESSON:
            config.lesson = (uint8_t)event->value;
            break;
        case UI_SETTING_LESSON_DURATION:
            config.duration_min = (uint8_t)event->value;
            break;
        case UI_SETTING_LESSON_CODE_WPM:
            config.code_wpm = (uint8_t)event->value;
            break;
        case UI_SETTING_LESSON_EFFECTIVE_WPM:
            config.effective_wpm = (uint8_t)event->value;
            break;
        case UI_SETTING_LESSON_GROUP_LEN:
            config.group_len = (uint8_t)event->value;
            break;
        case UI_SETTING_NONE:
        case UI_SETTING_VOLUME:
        case UI_SETTING_TONE_HZ:
        case UI_SETTING_KEY_IN_WPM:
        case UI_SETTING_KEY_IN_MODE:
        case UI_SETTING_WORD_SPEED:
        case UI_SETTING_WORD_MIN_CHAR_WPM:
        case UI_SETTING_WORD_LESSON:
        case UI_SETTING_WORD_MAX_LEN:
        case UI_SETTING_WORD_MAX_WPM:
        case UI_SETTING_WORD_DELAY_S:
        case UI_SETTING_CALLSIGN_SPEED:
        case UI_SETTING_CALLSIGN_MIN_CHAR_WPM:
        case UI_SETTING_CALLSIGN_MAX_WPM:
        case UI_SETTING_CALLSIGN_DELAY_S:
        case UI_SETTING_PLAINTEXT_CODE_WPM:
        case UI_SETTING_PLAINTEXT_EFFECTIVE_WPM:
        case UI_SETTING_USB_DRIVE:
        case UI_SETTING_KEYER_SK_WPM:
        default:
            break;
        }
    }

    /* UI reports intent; app_core applies trainer state and persistence routing. */
    cw_trainer_lesson_set_config(&config);
    storage_lesson_save_config(cw_trainer_lesson_get_config());
    ui_service_refresh();
}

static void app_core_handle_word_config_changed(const ui_input_event_t *event)
{
    cw_word_config_t config = *cw_trainer_word_get_config();

    if (event != NULL) {
        switch (event->setting) {
        case UI_SETTING_WORD_SPEED:
            config.start_wpm = (uint8_t)event->value;
            break;
        case UI_SETTING_WORD_MIN_CHAR_WPM:
            config.min_char_wpm = (uint8_t)event->value;
            break;
        case UI_SETTING_WORD_LESSON:
            config.lesson = (uint8_t)event->value;
            break;
        case UI_SETTING_WORD_MAX_LEN:
            config.max_word_len = (uint8_t)event->value;
            break;
        case UI_SETTING_WORD_MAX_WPM:
            config.max_wpm = (uint8_t)event->value;
            break;
        case UI_SETTING_WORD_DELAY_S:
            config.delay_s = (uint8_t)event->value;
            break;
        case UI_SETTING_NONE:
        case UI_SETTING_VOLUME:
        case UI_SETTING_TONE_HZ:
        case UI_SETTING_KEY_IN_WPM:
        case UI_SETTING_KEY_IN_MODE:
        case UI_SETTING_LESSON:
        case UI_SETTING_LESSON_DURATION:
        case UI_SETTING_LESSON_CODE_WPM:
        case UI_SETTING_LESSON_EFFECTIVE_WPM:
        case UI_SETTING_LESSON_GROUP_LEN:
        case UI_SETTING_CALLSIGN_SPEED:
        case UI_SETTING_CALLSIGN_MIN_CHAR_WPM:
        case UI_SETTING_CALLSIGN_MAX_WPM:
        case UI_SETTING_CALLSIGN_DELAY_S:
        case UI_SETTING_PLAINTEXT_CODE_WPM:
        case UI_SETTING_PLAINTEXT_EFFECTIVE_WPM:
        case UI_SETTING_USB_DRIVE:
        case UI_SETTING_KEYER_SK_WPM:
        default:
            break;
        }
    }

    /* UI reports intent; app_core applies trainer state and persistence routing. */
    cw_trainer_word_set_config(&config);
    storage_word_save_config(cw_trainer_word_get_config());
    ui_service_refresh();
}

static void app_core_handle_callsign_config_changed(const ui_input_event_t *event)
{
    cw_callsign_config_t config = *cw_trainer_callsign_get_config();

    if (event != NULL) {
        switch (event->setting) {
        case UI_SETTING_CALLSIGN_SPEED:
            config.start_wpm = (uint8_t)event->value;
            break;
        case UI_SETTING_CALLSIGN_MIN_CHAR_WPM:
            config.min_char_wpm = (uint8_t)event->value;
            break;
        case UI_SETTING_CALLSIGN_MAX_WPM:
            config.max_wpm = (uint8_t)event->value;
            break;
        case UI_SETTING_CALLSIGN_DELAY_S:
            config.delay_s = (uint8_t)event->value;
            break;
        case UI_SETTING_NONE:
        case UI_SETTING_VOLUME:
        case UI_SETTING_TONE_HZ:
        case UI_SETTING_KEY_IN_WPM:
        case UI_SETTING_KEY_IN_MODE:
        case UI_SETTING_LESSON:
        case UI_SETTING_LESSON_DURATION:
        case UI_SETTING_LESSON_CODE_WPM:
        case UI_SETTING_LESSON_EFFECTIVE_WPM:
        case UI_SETTING_LESSON_GROUP_LEN:
        case UI_SETTING_WORD_SPEED:
        case UI_SETTING_WORD_MIN_CHAR_WPM:
        case UI_SETTING_WORD_LESSON:
        case UI_SETTING_WORD_MAX_LEN:
        case UI_SETTING_WORD_MAX_WPM:
        case UI_SETTING_WORD_DELAY_S:
        case UI_SETTING_PLAINTEXT_CODE_WPM:
        case UI_SETTING_PLAINTEXT_EFFECTIVE_WPM:
        case UI_SETTING_USB_DRIVE:
        case UI_SETTING_KEYER_SK_WPM:
        default:
            break;
        }
    }

    /* UI reports intent; app_core applies trainer state and persistence routing. */
    cw_trainer_callsign_set_config(&config);
    storage_callsign_save_config(cw_trainer_callsign_get_config());
    ui_service_refresh();
}

static void app_core_handle_plaintext_config_changed(const ui_input_event_t *event)
{
    cw_plaintext_config_t config = *cw_trainer_plaintext_get_config();

    if (event != NULL) {
        switch (event->setting) {
        case UI_SETTING_PLAINTEXT_CODE_WPM:
            config.code_wpm = (uint8_t)event->value;
            break;
        case UI_SETTING_PLAINTEXT_EFFECTIVE_WPM:
            config.effective_wpm = (uint8_t)event->value;
            break;
        case UI_SETTING_NONE:
        case UI_SETTING_VOLUME:
        case UI_SETTING_TONE_HZ:
        case UI_SETTING_KEY_IN_WPM:
        case UI_SETTING_KEY_IN_MODE:
        case UI_SETTING_LESSON:
        case UI_SETTING_LESSON_DURATION:
        case UI_SETTING_LESSON_CODE_WPM:
        case UI_SETTING_LESSON_EFFECTIVE_WPM:
        case UI_SETTING_LESSON_GROUP_LEN:
        case UI_SETTING_WORD_SPEED:
        case UI_SETTING_WORD_MIN_CHAR_WPM:
        case UI_SETTING_WORD_LESSON:
        case UI_SETTING_WORD_MAX_LEN:
        case UI_SETTING_WORD_MAX_WPM:
        case UI_SETTING_WORD_DELAY_S:
        case UI_SETTING_CALLSIGN_SPEED:
        case UI_SETTING_CALLSIGN_MIN_CHAR_WPM:
        case UI_SETTING_CALLSIGN_MAX_WPM:
        case UI_SETTING_CALLSIGN_DELAY_S:
        case UI_SETTING_USB_DRIVE:
        case UI_SETTING_KEYER_SK_WPM:
        default:
            break;
        }
    }

    /* UI reports intent; app_core applies trainer state and persistence routing. */
    cw_trainer_plaintext_set_config(&config);
    storage_plaintext_save_config(cw_trainer_plaintext_get_config());
    ui_service_refresh();
}

static void app_core_handle_char_input(char key)
{
    if (s_app.mode == APP_MODE_PLAINTEXT) {
        cw_trainer_plaintext_append_char(key);
    } else if (s_app.mode == APP_MODE_LESSONS) {
        cw_trainer_lesson_append_char(key);
    } else if (s_app.mode == APP_MODE_WORDS) {
        cw_trainer_word_append_char(key);
    } else if (s_app.mode == APP_MODE_CALLSIGNS) {
        cw_trainer_callsign_append_char(key);
    } else if (s_app.mode == APP_MODE_KEYER && !s_keyer.tune_active) {
        app_core_keyer_append_tx_char(key);
    }

    ui_service_refresh();
}

static bool app_core_append_training_copy_char(char key)
{
    if (s_app.mode == APP_MODE_PLAINTEXT) {
        return cw_trainer_plaintext_append_char(key);
    }
    if (s_app.mode == APP_MODE_LESSONS) {
        return cw_trainer_lesson_append_char(key);
    }
    if (s_app.mode == APP_MODE_WORDS) {
        return cw_trainer_word_append_char(key);
    }
    if (s_app.mode == APP_MODE_CALLSIGNS) {
        return cw_trainer_callsign_append_char(key);
    }

    return false;
}

static bool app_core_append_training_space(void)
{
    if (s_app.mode == APP_MODE_WORDS || s_app.mode == APP_MODE_CALLSIGNS) {
        return false;
    }

    return app_core_append_training_copy_char(' ');
}

static bool app_core_handle_training_enter_from_keyer(void)
{
    if (s_app.mode == APP_MODE_WORDS) {
        app_core_handle_word_select();
        return false;
    }

    if (s_app.mode == APP_MODE_CALLSIGNS) {
        app_core_handle_callsign_select();
        return false;
    }

    return false;
}

static bool app_core_backspace_training_copy(void)
{
    if (s_app.mode == APP_MODE_PLAINTEXT) {
        const cw_plaintext_view_t *view = cw_trainer_plaintext_get_view();
        if (view != NULL && view->state == CW_PLAINTEXT_STATE_COPYING && view->copy_len > 0U) {
            cw_trainer_plaintext_backspace();
            return true;
        }
    } else if (s_app.mode == APP_MODE_LESSONS) {
        const cw_lesson_view_t *view = cw_trainer_lesson_get_view();
        if (view != NULL && view->state == CW_LESSON_STATE_COPYING && view->copy_len > 0U) {
            cw_trainer_lesson_backspace();
            return true;
        }
    } else if (s_app.mode == APP_MODE_WORDS) {
        const cw_word_view_t *view = cw_trainer_word_get_view();
        if (view != NULL && view->state == CW_WORD_STATE_COPYING && view->copy_len > 0U) {
            cw_trainer_word_backspace();
            return true;
        }
    } else if (s_app.mode == APP_MODE_CALLSIGNS) {
        const cw_callsign_view_t *view = cw_trainer_callsign_get_view();
        if (view != NULL && view->state == CW_CALLSIGN_STATE_COPYING && view->copy_len > 0U) {
            cw_trainer_callsign_backspace();
            return true;
        }
    }

    return false;
}

static bool app_core_handle_keyer_mode_decoded_event(const keyer_event_t *event)
{
    if (s_app.mode != APP_MODE_KEYER || event == NULL) {
        return false;
    }

    switch (event->type) {
    case KEYER_EVENT_CHAR_COMPLETE:
        app_core_keyer_log_append_char(event->decoded_char);
        keyer_service_op_feed_char(event->decoded_char);
        ui_service_keyer_append_decoded_char(event->decoded_char);
        return true;
    case KEYER_EVENT_WORD_SPACE:
        app_core_keyer_log_append_char(' ');
        keyer_service_op_feed_char(' ');
        ui_service_keyer_append_decoded_char(' ');
        return true;
    case KEYER_EVENT_BACKSPACE:
        app_core_keyer_log_backspace();
        ui_service_keyer_backspace_decoded();
        return true;
    case KEYER_EVENT_ENTER:
        return false;
    case KEYER_EVENT_TX_CANCELLED:
        s_keyer.tx_pending = false;
        app_core_keyer_cancel_repeat();
        s_keyer.last_append_was_message = false;
        app_core_keyer_sync_tx_display(true);
        return true;
    case KEYER_EVENT_DIT:
    case KEYER_EVENT_DAH:
        app_core_keyer_cancel_repeat();
        return false;
    case KEYER_EVENT_NONE:
    default:
        break;
    }

    return false;
}

static void app_core_handle_keyer_event(const keyer_event_t *event)
{
    bool handled = false;

    if (event == NULL || event->type == KEYER_EVENT_NONE) {
        return;
    }

    /* keyer_service owns paddle decoding; app_core only routes decoded intent. */
    if (s_app.mode == APP_MODE_KEYER) {
        handled = app_core_handle_keyer_mode_decoded_event(event);
    } else {
        switch (event->type) {
        case KEYER_EVENT_CHAR_COMPLETE:
            handled = app_core_append_training_copy_char(event->decoded_char);
            break;
        case KEYER_EVENT_WORD_SPACE:
            handled = app_core_append_training_space();
            break;
        case KEYER_EVENT_BACKSPACE:
            handled = app_core_backspace_training_copy();
            break;
        case KEYER_EVENT_ENTER:
            handled = app_core_handle_training_enter_from_keyer();
            break;
        case KEYER_EVENT_DIT:
        case KEYER_EVENT_DAH:
        case KEYER_EVENT_TX_CANCELLED:
        case KEYER_EVENT_NONE:
        default:
            break;
        }
    }

    if (handled) {
        ui_service_refresh();
    }
}

static void app_core_drain_keyer_events(void)
{
    for (;;) {
        keyer_event_t event = keyer_service_poll_event();
        if (event.type == KEYER_EVENT_NONE) {
            return;
        }

        app_core_handle_keyer_event(&event);
    }
}

static void app_core_handle_ui_event(ui_input_event_t event)
{
    if (event.type == UI_INPUT_EVENT_NONE) {
        return;
    }

    if (event.type == UI_INPUT_EVENT_CANCEL) {
        ESP_LOGI(TAG, "cancel input received");
        if (s_app.mode == APP_MODE_LESSONS) {
            cw_trainer_lesson_abort();
        } else if (s_app.mode == APP_MODE_PLAINTEXT) {
            cw_trainer_plaintext_abort();
        } else if (s_app.mode == APP_MODE_WORDS) {
            cw_trainer_word_abort();
        } else if (s_app.mode == APP_MODE_CALLSIGNS) {
            cw_trainer_callsign_abort();
        } else {
            if (s_keyer.tune_active) {
                app_core_keyer_set_tune_active(false);
            }
            app_core_keyer_clear_tx_fifo();
            audio_service_stop_all();
            cw_trainer_stop();
        }
        ui_service_refresh();
        return;
    }

    if (event.type == UI_INPUT_EVENT_SLEEP_REQUEST) {
        ESP_LOGI(TAG, "sleep input received");
        if (s_keyer.tune_active) {
            app_core_keyer_set_tune_active(false);
        }
        app_core_keyer_clear_tx_fifo();
        audio_service_stop_all();
        ui_service_prepare_for_sleep();
        vTaskDelay(pdMS_TO_TICKS(100));
        platform_hal_enter_deep_sleep();
        return;
    }

    switch (event.type) {
    case UI_INPUT_EVENT_MODE_CHANGED: {
        bool was_keyer = s_app.mode == APP_MODE_KEYER;
        if (s_keyer.tune_active) {
            app_core_keyer_set_tune_active(false);
        }
        app_core_sync_mode_from_ui();
        if (was_keyer && s_app.mode != APP_MODE_KEYER) {
            keyer_service_clear_op_name();
        }
        ui_service_refresh();
        break;
    }
    case UI_INPUT_EVENT_VOLUME_CHANGED:
        app_core_handle_volume_changed(&event);
        break;
    case UI_INPUT_EVENT_TONE_CHANGED:
        app_core_handle_tone_changed(&event);
        break;
    case UI_INPUT_EVENT_KEY_IN_WPM_CHANGED:
        app_core_handle_key_in_wpm_changed(&event);
        break;
    case UI_INPUT_EVENT_KEY_IN_MODE_CHANGED:
        app_core_handle_key_in_mode_changed(&event);
        break;
    case UI_INPUT_EVENT_DATETIME_CHANGED:
        app_core_handle_datetime_changed(&event);
        break;
    case UI_INPUT_EVENT_KEY_OUT_MODE_CHANGED:
        app_core_handle_key_out_mode_changed(&event);
        break;
    case UI_INPUT_EVENT_KEYER_PADDLE_MODE_CHANGED:
        app_core_handle_keyer_paddle_mode_changed(&event);
        break;
    case UI_INPUT_EVENT_KEYER_CONFIG_CHANGED:
        app_core_handle_keyer_config_changed(&event);
        break;
    case UI_INPUT_EVENT_KEYER_MUTE_CHANGED:
        app_core_handle_keyer_mute_changed(&event);
        break;
    case UI_INPUT_EVENT_KEYER_MACRO_SELECTED:
        app_core_keyer_append_message((uint8_t)event.value);
        ui_service_refresh();
        break;
    case UI_INPUT_EVENT_KEYER_SHORTCUT_CHANGED:
        ui_service_refresh();
        break;
    case UI_INPUT_EVENT_KEYER_CLEAR:
        if (s_app.mode == APP_MODE_KEYER) {
            app_core_keyer_clear_tx_fifo();
            ui_service_keyer_clear_decoded();
            ui_service_refresh();
        }
        break;
    case UI_INPUT_EVENT_KEYER_TUNE_CHANGED:
        if (s_app.mode == APP_MODE_KEYER) {
            app_core_keyer_set_tune_active(event.value != 0);
        }
        break;
    case UI_INPUT_EVENT_KEYER_TUNE_LATCH_CHANGED:
        if (s_app.mode == APP_MODE_KEYER) {
            app_core_keyer_set_tune_latched(event.value != 0);
        }
        break;
    case UI_INPUT_EVENT_LESSON_CONFIG_CHANGED:
        app_core_handle_lesson_config_changed(&event);
        break;
    case UI_INPUT_EVENT_WORD_CONFIG_CHANGED:
        app_core_handle_word_config_changed(&event);
        break;
    case UI_INPUT_EVENT_CALLSIGN_CONFIG_CHANGED:
        app_core_handle_callsign_config_changed(&event);
        break;
    case UI_INPUT_EVENT_PLAINTEXT_CONFIG_CHANGED:
        app_core_handle_plaintext_config_changed(&event);
        break;
    case UI_INPUT_EVENT_USB_DRIVE_CHANGED:
        app_core_handle_usb_drive_changed(&event);
        break;
    case UI_INPUT_EVENT_SELECT:
        if (s_app.mode == APP_MODE_PLAINTEXT) {
            app_core_handle_plaintext_select();
        } else if (s_app.mode == APP_MODE_LESSONS) {
            app_core_handle_lesson_select();
        } else if (s_app.mode == APP_MODE_WORDS) {
            app_core_handle_word_select();
        } else if (s_app.mode == APP_MODE_CALLSIGNS) {
            app_core_handle_callsign_select();
        } else if (s_app.mode == APP_MODE_KEYER) {
            app_core_keyer_start_tx_now();
            ui_service_refresh();
        }
        break;
    case UI_INPUT_EVENT_CHAR_INPUT:
        app_core_handle_char_input(event.key);
        break;
    case UI_INPUT_EVENT_BACKSPACE:
        if (s_app.mode == APP_MODE_PLAINTEXT) {
            cw_trainer_plaintext_backspace();
            ui_service_refresh();
        } else if (s_app.mode == APP_MODE_LESSONS) {
            cw_trainer_lesson_backspace();
            ui_service_refresh();
        } else if (s_app.mode == APP_MODE_WORDS) {
            cw_trainer_word_backspace();
            ui_service_refresh();
        } else if (s_app.mode == APP_MODE_CALLSIGNS) {
            cw_trainer_callsign_backspace();
            ui_service_refresh();
        } else if (s_app.mode == APP_MODE_KEYER) {
            app_core_keyer_backspace_tx();
            ui_service_refresh();
        }
        break;
    case UI_INPUT_EVENT_REPLAY:
        if (s_app.mode == APP_MODE_WORDS) {
            cw_trainer_word_replay();
            ui_service_refresh();
        } else if (s_app.mode == APP_MODE_CALLSIGNS) {
            cw_trainer_callsign_replay();
            ui_service_refresh();
        }
        break;
    case UI_INPUT_EVENT_NONE:
    case UI_INPUT_EVENT_CANCEL:
    case UI_INPUT_EVENT_SLEEP_REQUEST:
    default:
        break;
    }
}

void app_core_init(void)
{
    ESP_LOGI(TAG, "Mini-CW service initialization starting");

    ESP_LOGI(TAG, "init: platform_hal");
    platform_hal_init();

    ESP_LOGI(TAG, "init: storage_service");
    storage_service_init();

    ESP_LOGI(TAG, "init: audio_service");
    audio_service_init();

    ESP_LOGI(TAG, "init: keyer_service");
    keyer_service_init();

    ESP_LOGI(TAG, "init: ui_service");
    ui_service_init();

    ESP_LOGI(TAG, "init: cw_trainer_service");
    cw_trainer_service_init();
    app_core_load_persisted_settings();

    s_app.initialized = true;
    app_core_sync_mode_from_ui();
    ui_service_show_demo_screen();

    ESP_LOGI(TAG, "Mini-CW service initialization complete");
}

void app_core_run(void)
{
    if (!s_app.initialized) {
        app_core_init();
    }

    ESP_LOGI(TAG, "Mini-CW app loop started");

    for (;;) {
        keyer_service_update();
        cw_trainer_service_update();
        app_core_drain_keyer_events();
        ui_input_event_t event = ui_service_poll_input();
        app_core_handle_ui_event(event);
        app_core_keyer_update();
        app_core_maybe_refresh_system_time();
        app_core_maybe_save_dirty_config();
        vTaskDelay(app_core_ms_to_delay_ticks(APP_INPUT_POLL_MS));
    }
}

app_mode_t app_core_get_mode(void)
{
    return s_app.mode;
}

const char *app_core_mode_to_string(app_mode_t mode)
{
    switch (mode) {
    case APP_MODE_PLAINTEXT:
        return "Plain";
    case APP_MODE_KEYER:
        return "Keyer";
    case APP_MODE_LESSONS:
        return "Lessons";
    case APP_MODE_WORDS:
        return "Words";
    case APP_MODE_CALLSIGNS:
        return "Calls";
    case APP_MODE_SYSTEM:
        return "System";
    default:
        return "Unknown";
    }
}

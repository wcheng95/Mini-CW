/*
 * platform_hal
 *
 * Responsibility: Owns low-level board initialization and general platform
 * services.
 * Hardware ownership: RTC/time, battery/PMU, and shared platform setup.
 * Milestone 1 avoids board-specific driver calls until target details are set.
 */

#include "platform_hal.h"

#include "board_i2c.h"
#include "board_pins.h"
#include "board_power.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_sleep.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"

#include <stddef.h>

static const char *TAG = "platform_hal";

enum {
    PLATFORM_HAL_DS3231_ADDR = 0x68,
    PLATFORM_HAL_DS3231_SPEED_HZ = 100000,
    PLATFORM_HAL_DS3231_TIMEOUT_MS = 100,
    PLATFORM_HAL_DS3231_SECONDS_REG = 0x00,
    PLATFORM_HAL_DS3231_STATUS_REG = 0x0F,
    PLATFORM_HAL_DS3231_STATUS_OSF = 0x80,
    PLATFORM_HAL_SECONDS_PER_DAY = 86400,
    PLATFORM_HAL_US_PER_SECOND = 1000000,
};

static const platform_hal_datetime_t PLATFORM_HAL_DEFAULT_DATETIME = {
    .year = 2026,
    .month = 1,
    .day = 1,
    .hour = 0,
    .minute = 0,
    .second = 0,
    .source = PLATFORM_HAL_TIME_SOURCE_SOFTWARE,
};

static i2c_master_dev_handle_t s_ds3231;
static bool s_ds3231_available;
static bool s_time_valid;
static int64_t s_time_epoch_base;
static int64_t s_time_us_start;
static platform_hal_time_source_t s_time_source = PLATFORM_HAL_TIME_SOURCE_SOFTWARE;

static bool platform_hal_is_leap_year(uint16_t year)
{
    return (year % 4U == 0U) && ((year % 100U) != 0U || (year % 400U) == 0U);
}

static uint8_t platform_hal_days_in_month(uint16_t year, uint8_t month)
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

    if (month == 2U && platform_hal_is_leap_year(year)) {
        return 29U;
    }

    return days[month - 1U];
}

static bool platform_hal_datetime_valid(const platform_hal_datetime_t *datetime)
{
    if (datetime == NULL) {
        return false;
    }

    if (datetime->year < 2024U || datetime->year > 2099U) {
        return false;
    }

    if (datetime->month < 1U || datetime->month > 12U) {
        return false;
    }

    if (datetime->day < 1U ||
        datetime->day > platform_hal_days_in_month(datetime->year, datetime->month)) {
        return false;
    }

    return datetime->hour <= 23U && datetime->minute <= 59U && datetime->second <= 59U;
}

static int64_t platform_hal_datetime_to_epoch(const platform_hal_datetime_t *datetime)
{
    int64_t days = 0;

    for (uint16_t year = 1970U; year < datetime->year; ++year) {
        days += platform_hal_is_leap_year(year) ? 366 : 365;
    }

    for (uint8_t month = 1U; month < datetime->month; ++month) {
        days += platform_hal_days_in_month(datetime->year, month);
    }

    days += (int64_t)datetime->day - 1;

    return days * PLATFORM_HAL_SECONDS_PER_DAY + (int64_t)datetime->hour * 3600 +
           (int64_t)datetime->minute * 60 + datetime->second;
}

static void platform_hal_epoch_to_datetime(int64_t epoch,
                                           platform_hal_time_source_t source,
                                           platform_hal_datetime_t *out_datetime)
{
    uint16_t year = 1970U;
    uint8_t month = 1U;
    int64_t days;
    int64_t seconds;

    if (out_datetime == NULL) {
        return;
    }

    if (epoch < 0) {
        epoch = 0;
    }

    days = epoch / PLATFORM_HAL_SECONDS_PER_DAY;
    seconds = epoch % PLATFORM_HAL_SECONDS_PER_DAY;

    while (days >= (platform_hal_is_leap_year(year) ? 366 : 365)) {
        days -= platform_hal_is_leap_year(year) ? 366 : 365;
        ++year;
    }

    while (days >= platform_hal_days_in_month(year, month)) {
        days -= platform_hal_days_in_month(year, month);
        ++month;
    }

    out_datetime->year = year;
    out_datetime->month = month;
    out_datetime->day = (uint8_t)(days + 1);
    out_datetime->hour = (uint8_t)(seconds / 3600);
    seconds %= 3600;
    out_datetime->minute = (uint8_t)(seconds / 60);
    out_datetime->second = (uint8_t)(seconds % 60);
    out_datetime->source = source;
}

static void platform_hal_seed_software_time(const platform_hal_datetime_t *datetime,
                                            platform_hal_time_source_t source)
{
    platform_hal_datetime_t seeded = *datetime;

    if (source < PLATFORM_HAL_TIME_SOURCE_SOFTWARE || source > PLATFORM_HAL_TIME_SOURCE_GPS) {
        source = PLATFORM_HAL_TIME_SOURCE_SOFTWARE;
    }

    seeded.source = source;
    s_time_epoch_base = platform_hal_datetime_to_epoch(&seeded);
    s_time_us_start = esp_timer_get_time();
    s_time_source = source;
    s_time_valid = true;
}

static esp_err_t platform_hal_current_epoch(int64_t *out_epoch)
{
    int64_t elapsed_us;

    if (out_epoch == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_time_valid) {
        return ESP_ERR_INVALID_STATE;
    }

    elapsed_us = esp_timer_get_time() - s_time_us_start;
    if (elapsed_us < 0) {
        elapsed_us = 0;
    }

    *out_epoch = s_time_epoch_base + elapsed_us / PLATFORM_HAL_US_PER_SECOND;
    return ESP_OK;
}

static bool platform_hal_bcd_to_u8(uint8_t bcd, uint8_t *out_value)
{
    uint8_t high = (uint8_t)((bcd >> 4) & 0x0F);
    uint8_t low = (uint8_t)(bcd & 0x0F);

    if (out_value == NULL || high > 9U || low > 9U) {
        return false;
    }

    *out_value = (uint8_t)(high * 10U + low);
    return true;
}

static uint8_t platform_hal_u8_to_bcd(uint8_t value)
{
    return (uint8_t)(((value / 10U) << 4) | (value % 10U));
}

static esp_err_t platform_hal_ds3231_read_reg(uint8_t reg, uint8_t *data, size_t data_len)
{
    if (s_ds3231 == NULL || data == NULL || data_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    return i2c_master_transmit_receive(s_ds3231,
                                       &reg,
                                       sizeof(reg),
                                       data,
                                       data_len,
                                       PLATFORM_HAL_DS3231_TIMEOUT_MS);
}

static esp_err_t platform_hal_ds3231_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t data[] = {
        reg,
        value,
    };

    if (s_ds3231 == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return i2c_master_transmit(s_ds3231,
                               data,
                               sizeof(data),
                               PLATFORM_HAL_DS3231_TIMEOUT_MS);
}

static esp_err_t platform_hal_ds3231_read_datetime(platform_hal_datetime_t *out_datetime)
{
    uint8_t regs[7];
    uint8_t status = 0;
    uint8_t seconds;
    uint8_t minutes;
    uint8_t hours;
    uint8_t day;
    uint8_t month;
    uint8_t year;
    platform_hal_datetime_t datetime = {0};
    esp_err_t err;

    if (out_datetime == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = platform_hal_ds3231_read_reg(PLATFORM_HAL_DS3231_SECONDS_REG, regs, sizeof(regs));
    if (err != ESP_OK) {
        return err;
    }

    err = platform_hal_ds3231_read_reg(PLATFORM_HAL_DS3231_STATUS_REG, &status, sizeof(status));
    if (err != ESP_OK) {
        return err;
    }

    if ((regs[2] & 0x40U) != 0U) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (!platform_hal_bcd_to_u8((uint8_t)(regs[0] & 0x7FU), &seconds) ||
        !platform_hal_bcd_to_u8((uint8_t)(regs[1] & 0x7FU), &minutes) ||
        !platform_hal_bcd_to_u8((uint8_t)(regs[2] & 0x3FU), &hours) ||
        !platform_hal_bcd_to_u8((uint8_t)(regs[4] & 0x3FU), &day) ||
        !platform_hal_bcd_to_u8((uint8_t)(regs[5] & 0x1FU), &month) ||
        !platform_hal_bcd_to_u8(regs[6], &year)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if ((regs[5] & 0x80U) != 0U) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    datetime.year = (uint16_t)(2000U + year);
    datetime.month = month;
    datetime.day = day;
    datetime.hour = hours;
    datetime.minute = minutes;
    datetime.second = seconds;
    datetime.source = PLATFORM_HAL_TIME_SOURCE_DS3231;

    if (!platform_hal_datetime_valid(&datetime)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if ((status & PLATFORM_HAL_DS3231_STATUS_OSF) != 0U) {
        return ESP_ERR_INVALID_STATE;
    }

    *out_datetime = datetime;
    return ESP_OK;
}

static uint8_t platform_hal_datetime_day_of_week(const platform_hal_datetime_t *datetime)
{
    int64_t days = platform_hal_datetime_to_epoch(datetime) / PLATFORM_HAL_SECONDS_PER_DAY;

    return (uint8_t)(((days + 3) % 7) + 1);
}

static esp_err_t platform_hal_ds3231_write_datetime(const platform_hal_datetime_t *datetime)
{
    uint8_t data[8];
    uint8_t status = 0;
    esp_err_t err;

    if (s_ds3231 == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!platform_hal_datetime_valid(datetime)) {
        return ESP_ERR_INVALID_ARG;
    }

    data[0] = PLATFORM_HAL_DS3231_SECONDS_REG;
    data[1] = platform_hal_u8_to_bcd(datetime->second);
    data[2] = platform_hal_u8_to_bcd(datetime->minute);
    data[3] = platform_hal_u8_to_bcd(datetime->hour);
    data[4] = platform_hal_u8_to_bcd(platform_hal_datetime_day_of_week(datetime));
    data[5] = platform_hal_u8_to_bcd(datetime->day);
    data[6] = platform_hal_u8_to_bcd(datetime->month);
    data[7] = platform_hal_u8_to_bcd((uint8_t)(datetime->year - 2000U));

    err = i2c_master_transmit(s_ds3231, data, sizeof(data), PLATFORM_HAL_DS3231_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    err = platform_hal_ds3231_read_reg(PLATFORM_HAL_DS3231_STATUS_REG, &status, sizeof(status));
    if (err != ESP_OK) {
        return err;
    }

    status = (uint8_t)(status & ~PLATFORM_HAL_DS3231_STATUS_OSF);
    return platform_hal_ds3231_write_reg(PLATFORM_HAL_DS3231_STATUS_REG, status);
}

static void platform_hal_init_ds3231(void)
{
    i2c_master_bus_handle_t bus = NULL;
    uint8_t reg = PLATFORM_HAL_DS3231_SECONDS_REG;
    uint8_t seconds_reg = 0;
    platform_hal_datetime_t datetime;

    esp_err_t err = board_i2c_get_bus(&bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "DS3231 not detected at 0x%02x on I2C SDA=%d SCL=%d: %s",
                 PLATFORM_HAL_DS3231_ADDR,
                 BOARD_I2C_SDA,
                 BOARD_I2C_SCL,
                 esp_err_to_name(err));
        return;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PLATFORM_HAL_DS3231_ADDR,
        .scl_speed_hz = PLATFORM_HAL_DS3231_SPEED_HZ,
    };

    err = i2c_master_bus_add_device(bus, &dev_cfg, &s_ds3231);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "DS3231 not detected at 0x%02x on I2C SDA=%d SCL=%d: %s",
                 PLATFORM_HAL_DS3231_ADDR,
                 BOARD_I2C_SDA,
                 BOARD_I2C_SCL,
                 esp_err_to_name(err));
        return;
    }

    err = i2c_master_transmit_receive(s_ds3231,
                                      &reg,
                                      sizeof(reg),
                                      &seconds_reg,
                                      sizeof(seconds_reg),
                                      PLATFORM_HAL_DS3231_TIMEOUT_MS);
    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "DS3231 detected at 0x%02x on I2C SDA=%d SCL=%d seconds_reg=0x%02x",
                 PLATFORM_HAL_DS3231_ADDR,
                 BOARD_I2C_SDA,
                 BOARD_I2C_SCL,
                 seconds_reg);
        s_ds3231_available = true;
    } else {
        ESP_LOGW(TAG,
                 "DS3231 not detected at 0x%02x on I2C SDA=%d SCL=%d: %s",
                 PLATFORM_HAL_DS3231_ADDR,
                 BOARD_I2C_SDA,
                 BOARD_I2C_SCL,
                 esp_err_to_name(err));
        (void)i2c_master_bus_rm_device(s_ds3231);
        s_ds3231 = NULL;
        s_ds3231_available = false;
        return;
    }

    err = platform_hal_ds3231_read_datetime(&datetime);
    if (err == ESP_OK) {
        platform_hal_seed_software_time(&datetime, PLATFORM_HAL_TIME_SOURCE_DS3231);
        ESP_LOGI(TAG,
                 "DS3231 time loaded: %04u-%02u-%02u %02u:%02u:%02u",
                 (unsigned)datetime.year,
                 (unsigned)datetime.month,
                 (unsigned)datetime.day,
                 (unsigned)datetime.hour,
                 (unsigned)datetime.minute,
                 (unsigned)datetime.second);
    } else {
        ESP_LOGW(TAG, "DS3231 time not loaded; using software RTC: %s", esp_err_to_name(err));
    }
}

void platform_hal_init(void)
{
    platform_hal_seed_software_time(&PLATFORM_HAL_DEFAULT_DATETIME,
                                    PLATFORM_HAL_TIME_SOURCE_SOFTWARE);
    platform_hal_init_ds3231();
    ESP_LOGI(TAG, "initialized platform HAL");
}

esp_err_t platform_hal_get_datetime(platform_hal_datetime_t *out_datetime)
{
    int64_t epoch;
    esp_err_t err;

    if (out_datetime == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = platform_hal_current_epoch(&epoch);
    if (err != ESP_OK) {
        return err;
    }

    platform_hal_epoch_to_datetime(epoch, s_time_source, out_datetime);
    return ESP_OK;
}

esp_err_t platform_hal_set_datetime(const platform_hal_datetime_t *datetime,
                                    platform_hal_time_source_t source)
{
    esp_err_t err;

    if (!platform_hal_datetime_valid(datetime)) {
        return ESP_ERR_INVALID_ARG;
    }

    platform_hal_seed_software_time(datetime, source);

    if (s_ds3231_available) {
        err = platform_hal_ds3231_write_datetime(datetime);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "DS3231 time update failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG,
                     "DS3231 time updated: %04u-%02u-%02u %02u:%02u:%02u",
                     (unsigned)datetime->year,
                     (unsigned)datetime->month,
                     (unsigned)datetime->day,
                     (unsigned)datetime->hour,
                     (unsigned)datetime->minute,
                     (unsigned)datetime->second);
        }
    }

    return ESP_OK;
}

platform_hal_time_source_t platform_hal_get_time_source(void)
{
    return s_time_source;
}

bool platform_hal_ds3231_available(void)
{
    return s_ds3231_available;
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

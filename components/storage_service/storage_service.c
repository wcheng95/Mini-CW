/*
 * storage_service
 *
 * Responsibility: Owns future profile, lesson, and session log persistence.
 * Hardware ownership: SD/SPIFFS/FATFS/file access and USB MSC exposure.
 */

#include "storage_service.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "tinyusb.h"
#include "tusb_msc_storage.h"
#include "wear_levelling.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "storage_service";

#define STORAGE_FATFS_LABEL "fatfs"
#define STORAGE_FATFS_BASE_PATH "/fatfs"
#define STORAGE_SETTINGS_PATH STORAGE_FATFS_BASE_PATH "/setting.txt"
#define STORAGE_SETTINGS_TMP_PATH STORAGE_FATFS_BASE_PATH "/setting.tmp"
#define STORAGE_QSOCALLS_PATH STORAGE_FATFS_BASE_PATH "/qsocalls.csv"
#define STORAGE_KEYER_LOG_PATH STORAGE_FATFS_BASE_PATH "/keyerlog.txt"
#define STORAGE_QSOCALLS_HEADER "call,name\n"
#define STORAGE_FATFS_MAX_FILES 4
#define STORAGE_FATFS_ALLOC_UNIT 512
#define STORAGE_SETTINGS_LINE_MAX 128
#define STORAGE_QSOCALLS_LINE_MAX 128
#define STORAGE_VOLUME_MIN 0U
#define STORAGE_VOLUME_MAX 99U
#define STORAGE_TONE_HZ_MIN 300U
#define STORAGE_TONE_HZ_MAX 999U
#define STORAGE_KEY_WPM_MIN 5U
#define STORAGE_KEY_WPM_MAX 60U
#define STORAGE_LESSON_MIN 1U
#define STORAGE_LESSON_MAX 40U
#define STORAGE_LESSON_DURATION_MIN 1U
#define STORAGE_LESSON_DURATION_MAX 5U
#define STORAGE_LESSON_WPM_MIN 5U
#define STORAGE_LESSON_WPM_MAX 40U
#define STORAGE_LESSON_GROUP_MIN 2U
#define STORAGE_LESSON_GROUP_MAX 7U
#define STORAGE_WORD_WPM_MIN 5U
#define STORAGE_WORD_WPM_MAX 40U
#define STORAGE_WORD_LESSON_MIN 9U
#define STORAGE_WORD_LESSON_MAX 40U
#define STORAGE_WORD_MAX_LEN_MIN 2U
#define STORAGE_WORD_MAX_LEN_MAX 15U
#define STORAGE_CALLSIGN_WPM_MIN 5U
#define STORAGE_CALLSIGN_WPM_MAX 40U
#define STORAGE_DELAY_S_MIN 0U
#define STORAGE_DELAY_S_MAX 5U
#define STORAGE_PLAINTEXT_WPM_MIN 5U
#define STORAGE_PLAINTEXT_WPM_MAX 40U
#define STORAGE_KEYER_TX_DELAY_MIN 0U
#define STORAGE_KEYER_TX_DELAY_MAX 99U
#define STORAGE_KEYER_TUNE_TIMEOUT_MIN 0U
#define STORAGE_KEYER_TUNE_TIMEOUT_MAX 20U
#define STORAGE_KEYER_REPEAT_MIN 1U
#define STORAGE_KEYER_REPEAT_MAX 99U
#define STORAGE_KEYER_SK_WPM_MIN 5U
#define STORAGE_KEYER_SK_WPM_MAX 60U
#define STORAGE_KEYER_DEFAULT_M1 "CQ POTA"
#define STORAGE_KEYER_DEFAULT_M2 ""
#define STORAGE_KEYER_DEFAULT_M3 ""
#define STORAGE_KEYER_DEFAULT_M4 ""
#define STORAGE_KEYER_DEFAULT_M5 ""
#define STORAGE_KEYER_PREVIOUS_DEFAULT_M1 "CQ SOTA DE AG6AQ"
#define STORAGE_KEYER_PREVIOUS_DEFAULT_M2 "TU UR CA CA BK"
#define STORAGE_KEYER_PREVIOUS_DEFAULT_M3 "BK TU 72 DE AG6AQ E E"
#define STORAGE_KEYER_PREVIOUS_DEFAULT_M4 "AG6AQ"
#define STORAGE_KEYER_PREVIOUS_DEFAULT_M5 "BK TU GM UR 599 599 CA CA BK"
#define STORAGE_KEYER_OLD_DEFAULT_M2 "[" "CALL] TU [" "OP] UR [" "599]*2 CA CA BK"
#define STORAGE_KEYER_OLD_DEFAULT_M5 "BK TU GM UR [" "599]*2 CA CA BK"
#define STORAGE_KEYER_DEFAULT_MYCALL "AG6AQ"
#define STORAGE_SYSTEM_DEFAULT_DATE "2026-01-01"
#define STORAGE_SYSTEM_DEFAULT_TIME "00:00:00"
#define STORAGE_GPS_BAUD_DEFAULT 115200
#define STORAGE_GPS_BAUD_SLOW 9600

#define STORAGE_KEY_SYSTEM_VOLUME (1UL << 0)
#define STORAGE_KEY_SYSTEM_KEY_IN (1UL << 1)
#define STORAGE_KEY_SYSTEM_KEY_IN_WPM (1UL << 2)
#define STORAGE_KEY_SYSTEM_USB_DRIVE (1UL << 3)
#define STORAGE_KEY_LESSON_LESSON (1UL << 4)
#define STORAGE_KEY_LESSON_DURATION (1UL << 5)
#define STORAGE_KEY_LESSON_CODE_WPM (1UL << 6)
#define STORAGE_KEY_LESSON_EFFECTIVE_WPM (1UL << 7)
#define STORAGE_KEY_LESSON_GROUP_LEN (1UL << 8)
#define STORAGE_KEY_WORD_SPEED (1UL << 9)
#define STORAGE_KEY_WORD_MIN_CHAR_WPM (1UL << 10)
#define STORAGE_KEY_WORD_LESSON (1UL << 11)
#define STORAGE_KEY_WORD_MAX_LEN (1UL << 12)
#define STORAGE_KEY_CALLSIGN_SPEED (1UL << 13)
#define STORAGE_KEY_CALLSIGN_MIN_CHAR_WPM (1UL << 14)
#define STORAGE_KEY_CALLSIGN_MAX_WPM (1UL << 15)
#define STORAGE_KEY_PLAINTEXT_CODE_WPM (1UL << 16)
#define STORAGE_KEY_PLAINTEXT_EFFECTIVE_WPM (1UL << 17)
#define STORAGE_KEY_SYSTEM_TONE_HZ (1UL << 18)
#define STORAGE_KEY_WORD_MAX_WPM (1UL << 19)
#define STORAGE_KEY_WORD_DELAY_S (1UL << 20)
#define STORAGE_KEY_CALLSIGN_DELAY_S (1UL << 21)
#define STORAGE_KEY_KEYER_KEY_OUT (1UL << 22)
#define STORAGE_KEY_KEYER_PADDLE (1UL << 23)
#define STORAGE_KEY_KEYER_TX_DELAY (1UL << 24)
#define STORAGE_KEY_KEYER_REPEAT (1UL << 25)
#define STORAGE_KEY_KEYER_M1 (1UL << 26)
#define STORAGE_KEY_KEYER_M2 (1UL << 27)
#define STORAGE_KEY_KEYER_M3 (1UL << 28)
#define STORAGE_KEY_KEYER_M4 (1UL << 29)
#define STORAGE_KEY_KEYER_M5 (1UL << 30)
#define STORAGE_KEY_KEYER_TUNE_TIMEOUT (1UL << 31)
#define STORAGE_KEY_KEYER_MYCALL (1ULL << 32)
#define STORAGE_KEY_KEYER_SK_WPM (1ULL << 33)
#define STORAGE_KEY_SYSTEM_DATE (1ULL << 34)
#define STORAGE_KEY_SYSTEM_TIME (1ULL << 35)
#define STORAGE_KEY_SYSTEM_GPS_BAUD (1ULL << 36)

#define STORAGE_SECTION_SYSTEM (1UL << 0)
#define STORAGE_SECTION_KEYER (1UL << 1)
#define STORAGE_SECTION_LESSONS (1UL << 2)
#define STORAGE_SECTION_WORDS (1UL << 3)
#define STORAGE_SECTION_CALLS (1UL << 4)
#define STORAGE_SECTION_PLAIN (1UL << 5)

#define STORAGE_EXPECTED_KEYS                                                        \
    (STORAGE_KEY_SYSTEM_VOLUME | STORAGE_KEY_SYSTEM_KEY_IN |                         \
     STORAGE_KEY_SYSTEM_TONE_HZ | STORAGE_KEY_SYSTEM_KEY_IN_WPM |                    \
     STORAGE_KEY_SYSTEM_USB_DRIVE | STORAGE_KEY_SYSTEM_DATE |                        \
     STORAGE_KEY_SYSTEM_TIME | STORAGE_KEY_SYSTEM_GPS_BAUD |                         \
     STORAGE_KEY_LESSON_LESSON | STORAGE_KEY_LESSON_DURATION |                       \
     STORAGE_KEY_LESSON_CODE_WPM | STORAGE_KEY_LESSON_EFFECTIVE_WPM |               \
     STORAGE_KEY_LESSON_GROUP_LEN | STORAGE_KEY_WORD_SPEED |                        \
     STORAGE_KEY_WORD_MIN_CHAR_WPM | STORAGE_KEY_WORD_LESSON |                      \
      STORAGE_KEY_WORD_MAX_LEN | STORAGE_KEY_WORD_MAX_WPM |                          \
      STORAGE_KEY_WORD_DELAY_S | STORAGE_KEY_CALLSIGN_SPEED |                        \
      STORAGE_KEY_CALLSIGN_MIN_CHAR_WPM | STORAGE_KEY_CALLSIGN_MAX_WPM |             \
      STORAGE_KEY_CALLSIGN_DELAY_S |                                                 \
      STORAGE_KEY_KEYER_KEY_OUT | STORAGE_KEY_KEYER_PADDLE |                         \
      STORAGE_KEY_KEYER_TX_DELAY | STORAGE_KEY_KEYER_REPEAT | STORAGE_KEY_KEYER_M1 |  \
      STORAGE_KEY_KEYER_M2 | STORAGE_KEY_KEYER_M3 | STORAGE_KEY_KEYER_M4 |            \
      STORAGE_KEY_KEYER_M5 | STORAGE_KEY_KEYER_TUNE_TIMEOUT | STORAGE_KEY_KEYER_MYCALL | \
      STORAGE_KEY_KEYER_SK_WPM |                                                      \
      STORAGE_KEY_PLAINTEXT_CODE_WPM | STORAGE_KEY_PLAINTEXT_EFFECTIVE_WPM)

#define STORAGE_EXPECTED_SECTIONS                                                    \
    (STORAGE_SECTION_SYSTEM | STORAGE_SECTION_KEYER | STORAGE_SECTION_LESSONS |      \
     STORAGE_SECTION_WORDS | STORAGE_SECTION_CALLS | STORAGE_SECTION_PLAIN)

typedef enum {
    STORAGE_SETTING_SECTION_NONE = 0,
    STORAGE_SETTING_SECTION_UNKNOWN,
    STORAGE_SETTING_SECTION_SYSTEM,
    STORAGE_SETTING_SECTION_KEYER,
    STORAGE_SETTING_SECTION_LESSONS,
    STORAGE_SETTING_SECTION_WORDS,
    STORAGE_SETTING_SECTION_CALLS,
    STORAGE_SETTING_SECTION_PLAIN,
} storage_setting_section_t;

static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;
static bool s_storage_ready;
static bool s_fatfs_mounted;
static bool s_usb_drive_enabled;
static bool s_tinyusb_installed;
static bool s_settings_loaded;

static storage_system_config_t s_system_config;
static keyer_config_t s_keyer_config;
static cw_lesson_config_t s_lesson_config;
static cw_word_config_t s_word_config;
static cw_callsign_config_t s_callsign_config;
static cw_plaintext_config_t s_plaintext_config;

static void storage_mount_changed_cb(tinyusb_msc_event_t *event)
{
    if (event == NULL) {
        return;
    }

    ESP_LOGI(TAG,
             "USB MSC app mount changed: mounted=%s",
             event->mount_changed_data.is_mounted ? "yes" : "no");
}

static esp_err_t storage_mount_fatfs(void)
{
    esp_err_t err;

    if (!s_storage_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_fatfs_mounted) {
        return ESP_OK;
    }

    err = tinyusb_msc_storage_mount(STORAGE_FATFS_BASE_PATH);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount FATFS at %s failed: %s", STORAGE_FATFS_BASE_PATH, esp_err_to_name(err));
        return err;
    }

    s_fatfs_mounted = true;
    ESP_LOGI(TAG, "FATFS mounted at %s", STORAGE_FATFS_BASE_PATH);
    return ESP_OK;
}

static esp_err_t storage_unmount_fatfs(void)
{
    esp_err_t err;

    if (!s_storage_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_fatfs_mounted) {
        return ESP_OK;
    }

    err = tinyusb_msc_storage_unmount();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "unmount FATFS failed: %s", esp_err_to_name(err));
        return err;
    }

    s_fatfs_mounted = false;
    ESP_LOGI(TAG, "FATFS unmounted from firmware");
    return ESP_OK;
}

static esp_err_t storage_install_tinyusb(void)
{
    esp_err_t err;

    if (s_tinyusb_installed) {
        return ESP_OK;
    }

    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = NULL,
        .string_descriptor = NULL,
        .string_descriptor_count = 0,
        .external_phy = false,
#if (TUD_OPT_HIGH_SPEED)
        .fs_configuration_descriptor = NULL,
        .hs_configuration_descriptor = NULL,
        .qualifier_descriptor = NULL,
#else
        .configuration_descriptor = NULL,
#endif
    };

    err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB MSC install failed: %s", esp_err_to_name(err));
        return err;
    }

    s_tinyusb_installed = true;
    ESP_LOGI(TAG, "TinyUSB MSC installed");
    return ESP_OK;
}

static esp_err_t storage_uninstall_tinyusb(void)
{
    esp_err_t err;

    if (!s_tinyusb_installed) {
        return ESP_OK;
    }

    err = tinyusb_driver_uninstall();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB MSC uninstall failed: %s", esp_err_to_name(err));
        return err;
    }

    s_tinyusb_installed = false;
    ESP_LOGI(TAG, "TinyUSB MSC uninstalled");
    return ESP_OK;
}

static bool storage_firmware_can_access_fatfs(const char *operation)
{
    if (s_usb_drive_enabled) {
        ESP_LOGW(TAG, "%s skipped: USB Drive is ON and PC owns FATFS", operation);
        return false;
    }

    if (!s_fatfs_mounted) {
        ESP_LOGW(TAG, "%s skipped: FATFS is not mounted", operation);
        return false;
    }

    return true;
}

static uint8_t storage_clamp_u8(uint8_t value, uint8_t min_value, uint8_t max_value)
{
    if (value < min_value) {
        return min_value;
    }

    if (value > max_value) {
        return max_value;
    }

    return value;
}

static uint8_t storage_clamp_u32_to_u8(uint32_t value,
                                       uint8_t min_value,
                                       uint8_t max_value,
                                       bool *changed)
{
    if (value < min_value) {
        if (changed != NULL) {
            *changed = true;
        }
        return min_value;
    }

    if (value > max_value) {
        if (changed != NULL) {
            *changed = true;
        }
        return max_value;
    }

    return (uint8_t)value;
}

static bool storage_str_equal_ignore_case(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }

    while (*a != '\0' && *b != '\0') {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        ++a;
        ++b;
    }

    return *a == '\0' && *b == '\0';
}

static char *storage_trim(char *text)
{
    char *end;

    if (text == NULL) {
        return NULL;
    }

    while (isspace((unsigned char)*text)) {
        ++text;
    }

    if ((unsigned char)text[0] == 0xEFU && (unsigned char)text[1] == 0xBBU &&
        (unsigned char)text[2] == 0xBFU) {
        text += 3;
        while (isspace((unsigned char)*text)) {
            ++text;
        }
    }

    if (*text == '\0') {
        return text;
    }

    end = text + strlen(text) - 1U;
    while (end > text && isspace((unsigned char)*end)) {
        *end = '\0';
        --end;
    }

    return text;
}

static void storage_copy_keyer_message(char *destination, const char *source)
{
    if (destination == NULL) {
        return;
    }

    snprintf(destination,
             KEYER_MESSAGE_MAX_LEN + 1U,
             "%.*s",
             (int)KEYER_MESSAGE_MAX_LEN,
             source ? source : "");
}

static void storage_copy_keyer_mycall(char *destination, const char *source)
{
    size_t out = 0U;

    if (destination == NULL) {
        return;
    }

    if (source == NULL) {
        destination[0] = '\0';
        return;
    }

    while (*source != '\0' && out < KEYER_MYCALL_MAX_LEN) {
        char ch = (char)toupper((unsigned char)*source++);
        if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '/') {
            destination[out++] = ch;
        }
    }
    destination[out] = '\0';
}

static void storage_strip_inline_comment(char *line)
{
    if (line == NULL) {
        return;
    }

    for (char *cursor = line; *cursor != '\0'; ++cursor) {
        if (*cursor == '#' || *cursor == ';') {
            *cursor = '\0';
            return;
        }
    }
}

static bool storage_parse_u32(const char *value, uint32_t *out_value)
{
    char *end = NULL;
    unsigned long parsed;

    if (value == NULL || out_value == NULL || *value == '\0' || *value == '-') {
        return false;
    }

    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (end == value || errno != 0 || parsed > UINT32_MAX) {
        return false;
    }

    end = storage_trim(end);
    if (end == NULL || *end != '\0') {
        return false;
    }

    *out_value = (uint32_t)parsed;
    return true;
}

static bool storage_parse_bool(const char *value, bool *out_value)
{
    if (value == NULL || out_value == NULL) {
        return false;
    }

    if (storage_str_equal_ignore_case(value, "on") ||
        storage_str_equal_ignore_case(value, "true") ||
        storage_str_equal_ignore_case(value, "yes") ||
        storage_str_equal_ignore_case(value, "1")) {
        *out_value = true;
        return true;
    }

    if (storage_str_equal_ignore_case(value, "off") ||
        storage_str_equal_ignore_case(value, "false") ||
        storage_str_equal_ignore_case(value, "no") ||
        storage_str_equal_ignore_case(value, "0")) {
        *out_value = false;
        return true;
    }

    return false;
}

static const char *storage_key_in_mode_label(keyer_key_in_mode_t mode)
{
    switch (mode) {
    case KEYER_KEY_IN_PADDLE:
        return "Pdl";
    case KEYER_KEY_IN_PADDLE_R:
        return "Pdl-R";
    case KEYER_KEY_IN_SK_T:
        return "SK-T";
    case KEYER_KEY_IN_SK_R:
        return "SK-R";
    default:
        return "Pdl";
    }
}

static bool storage_parse_key_in_mode(const char *value, keyer_key_in_mode_t *out_mode)
{
    uint32_t numeric_value;

    if (value == NULL || out_mode == NULL) {
        return false;
    }

    if (storage_str_equal_ignore_case(value, "Pdl") ||
        storage_str_equal_ignore_case(value, "Paddle")) {
        *out_mode = KEYER_KEY_IN_PADDLE;
        return true;
    }

    if (storage_str_equal_ignore_case(value, "Pdl-R") ||
        storage_str_equal_ignore_case(value, "PdlR") ||
        storage_str_equal_ignore_case(value, "PaddleR") ||
        storage_str_equal_ignore_case(value, "Paddle-R") ||
        storage_str_equal_ignore_case(value, "Paddle_R")) {
        *out_mode = KEYER_KEY_IN_PADDLE_R;
        return true;
    }

    if (storage_str_equal_ignore_case(value, "SK-T") ||
        storage_str_equal_ignore_case(value, "SKT") ||
        storage_str_equal_ignore_case(value, "SK")) {
        *out_mode = KEYER_KEY_IN_SK_T;
        return true;
    }

    if (storage_str_equal_ignore_case(value, "SK-R") ||
        storage_str_equal_ignore_case(value, "SKR") ||
        storage_str_equal_ignore_case(value, "SK-Mono") ||
        storage_str_equal_ignore_case(value, "SK_Mono") ||
        storage_str_equal_ignore_case(value, "SKMono")) {
        *out_mode = KEYER_KEY_IN_SK_R;
        return true;
    }

    if (storage_parse_u32(value, &numeric_value) && numeric_value <= (uint32_t)KEYER_KEY_IN_SK_R) {
        *out_mode = (keyer_key_in_mode_t)numeric_value;
        return true;
    }

    return false;
}

static const char *storage_key_out_mode_label(keyer_key_out_mode_t mode)
{
    return keyer_service_key_out_mode_label(mode);
}

static bool storage_parse_key_out_mode(const char *value, keyer_key_out_mode_t *out_mode)
{
    uint32_t numeric_value;

    if (value == NULL || out_mode == NULL) {
        return false;
    }

    if (storage_str_equal_ignore_case(value, "Pdl") ||
        storage_str_equal_ignore_case(value, "Paddle")) {
        *out_mode = KEYER_KEY_OUT_PADDLE;
        return true;
    }

    if (storage_str_equal_ignore_case(value, "Pdl-R") ||
        storage_str_equal_ignore_case(value, "PdlR") ||
        storage_str_equal_ignore_case(value, "PaddleR") ||
        storage_str_equal_ignore_case(value, "Paddle-R") ||
        storage_str_equal_ignore_case(value, "Paddle_R")) {
        *out_mode = KEYER_KEY_OUT_PADDLE_R;
        return true;
    }

    if (storage_str_equal_ignore_case(value, "SK")) {
        *out_mode = KEYER_KEY_OUT_SK;
        return true;
    }

    if (storage_str_equal_ignore_case(value, "SK-M") ||
        storage_str_equal_ignore_case(value, "SKM") ||
        storage_str_equal_ignore_case(value, "SK-Mono") ||
        storage_str_equal_ignore_case(value, "SKMono")) {
        *out_mode = KEYER_KEY_OUT_SK_M;
        return true;
    }

    if (storage_str_equal_ignore_case(value, "OFF")) {
        *out_mode = KEYER_KEY_OUT_OFF;
        return true;
    }

    if (storage_parse_u32(value, &numeric_value) &&
        numeric_value <= (uint32_t)KEYER_KEY_OUT_OFF) {
        *out_mode = (keyer_key_out_mode_t)numeric_value;
        return true;
    }

    return false;
}

static bool storage_parse_paddle_mode(const char *value, keyer_paddle_mode_t *out_mode)
{
    uint32_t numeric_value;

    if (value == NULL || out_mode == NULL) {
        return false;
    }

    if (storage_str_equal_ignore_case(value, "IambicA") ||
        storage_str_equal_ignore_case(value, "Iambic-A")) {
        *out_mode = KEYER_PADDLE_IAMBIC_A;
        return true;
    }

    if (storage_str_equal_ignore_case(value, "IambicB") ||
        storage_str_equal_ignore_case(value, "Iambic-B")) {
        *out_mode = KEYER_PADDLE_IAMBIC_B;
        return true;
    }

    if (storage_str_equal_ignore_case(value, "Bug")) {
        *out_mode = KEYER_PADDLE_BUG;
        return true;
    }

    if (storage_parse_u32(value, &numeric_value) && numeric_value <= (uint32_t)KEYER_PADDLE_BUG) {
        *out_mode = (keyer_paddle_mode_t)numeric_value;
        return true;
    }

    return false;
}

static void storage_settings_set_defaults(void)
{
    s_system_config = (storage_system_config_t){
        .volume = 80,
        .tone_hz = 700,
        .key_in_mode = KEYER_KEY_IN_PADDLE,
        .key_in_wpm = 19,
        .gps_baud = STORAGE_GPS_BAUD_DEFAULT,
        .date = STORAGE_SYSTEM_DEFAULT_DATE,
        .time = STORAGE_SYSTEM_DEFAULT_TIME,
    };

    s_keyer_config = (keyer_config_t){
        .key_out_mode = KEYER_KEY_OUT_SK,
        .paddle_mode = KEYER_PADDLE_IAMBIC_A,
        .sk_wpm = 19,
        .tx_delay_s = 0,
        .tune_timeout_s = 10,
        .repeat_interval_s = 6,
        .mycall = STORAGE_KEYER_DEFAULT_MYCALL,
        .message = {
            STORAGE_KEYER_DEFAULT_M1,
            STORAGE_KEYER_DEFAULT_M2,
            STORAGE_KEYER_DEFAULT_M3,
            STORAGE_KEYER_DEFAULT_M4,
            STORAGE_KEYER_DEFAULT_M5,
        },
    };

    s_lesson_config = (cw_lesson_config_t){
        .lesson = 1,
        .duration_min = 1,
        .code_wpm = 20,
        .effective_wpm = 12,
        .group_len = 0,
    };

    s_word_config = (cw_word_config_t){
        .start_wpm = 20,
        .min_char_wpm = 10,
        .lesson = 40,
        .max_word_len = 15,
        .max_wpm = 30,
        .delay_s = 1,
    };

    s_callsign_config = (cw_callsign_config_t){
        .start_wpm = 20,
        .min_char_wpm = 10,
        .max_wpm = 30,
        .delay_s = 1,
    };

    s_plaintext_config = (cw_plaintext_config_t){
        .code_wpm = 20,
        .effective_wpm = 12,
    };
}

static bool storage_is_leap_year(uint16_t year)
{
    return (year % 4U == 0U) && ((year % 100U) != 0U || (year % 400U) == 0U);
}

static uint8_t storage_days_in_month(uint16_t year, uint8_t month)
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

    if (month == 2U && storage_is_leap_year(year)) {
        return 29U;
    }

    return days[month - 1U];
}

static bool storage_parse_fixed_uint(const char *text,
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

static bool storage_date_string_valid(const char *date)
{
    uint16_t year;
    uint16_t month;
    uint16_t day;

    if (date == NULL || strlen(date) != STORAGE_SYSTEM_DATE_LEN) {
        return false;
    }

    if (date[4] != '-' || date[7] != '-') {
        return false;
    }

    if (!storage_parse_fixed_uint(date, 0U, 4U, &year) ||
        !storage_parse_fixed_uint(date, 5U, 2U, &month) ||
        !storage_parse_fixed_uint(date, 8U, 2U, &day)) {
        return false;
    }

    if (year < 2024U || year > 2099U || month < 1U || month > 12U || day < 1U) {
        return false;
    }

    return day <= storage_days_in_month(year, (uint8_t)month);
}

static bool storage_time_string_valid(const char *time)
{
    uint16_t hour;
    uint16_t minute;
    uint16_t second;

    if (time == NULL || strlen(time) != STORAGE_SYSTEM_TIME_LEN) {
        return false;
    }

    if (time[2] != ':' || time[5] != ':') {
        return false;
    }

    if (!storage_parse_fixed_uint(time, 0U, 2U, &hour) ||
        !storage_parse_fixed_uint(time, 3U, 2U, &minute) ||
        !storage_parse_fixed_uint(time, 6U, 2U, &second)) {
        return false;
    }

    return hour <= 23U && minute <= 59U && second <= 59U;
}

static bool storage_apply_datetime_string(const char *value,
                                           char *target,
                                           size_t target_size,
                                          bool date_value,
                                          bool *changed)
{
    bool valid = date_value ? storage_date_string_valid(value) : storage_time_string_valid(value);

    if (target == NULL || target_size == 0U || !valid) {
        if (changed != NULL) {
            *changed = true;
        }
        return false;
    }

    snprintf(target, target_size, "%s", value);
    return true;
}

static int storage_normalize_gps_baud(int value)
{
    return value == STORAGE_GPS_BAUD_SLOW ? STORAGE_GPS_BAUD_SLOW : STORAGE_GPS_BAUD_DEFAULT;
}

static void storage_normalize_system_config(bool *changed)
{
    uint8_t normalized_volume =
        storage_clamp_u8(s_system_config.volume, STORAGE_VOLUME_MIN, STORAGE_VOLUME_MAX);
    uint16_t normalized_tone_hz = s_system_config.tone_hz;
    uint8_t normalized_wpm =
        storage_clamp_u8(s_system_config.key_in_wpm, STORAGE_KEY_WPM_MIN, STORAGE_KEY_WPM_MAX);
    keyer_key_in_mode_t normalized_mode = s_system_config.key_in_mode;
    int normalized_gps_baud = storage_normalize_gps_baud(s_system_config.gps_baud);
    char normalized_date[STORAGE_SYSTEM_DATE_LEN + 1U];
    char normalized_time[STORAGE_SYSTEM_TIME_LEN + 1U];

    snprintf(normalized_date,
             sizeof(normalized_date),
             "%s",
             storage_date_string_valid(s_system_config.date) ? s_system_config.date
                                                             : STORAGE_SYSTEM_DEFAULT_DATE);
    snprintf(normalized_time,
             sizeof(normalized_time),
             "%s",
             storage_time_string_valid(s_system_config.time) ? s_system_config.time
                                                             : STORAGE_SYSTEM_DEFAULT_TIME);

    if (normalized_tone_hz < STORAGE_TONE_HZ_MIN) {
        normalized_tone_hz = STORAGE_TONE_HZ_MIN;
    } else if (normalized_tone_hz > STORAGE_TONE_HZ_MAX) {
        normalized_tone_hz = STORAGE_TONE_HZ_MAX;
    }

    if ((int)normalized_mode < (int)KEYER_KEY_IN_PADDLE ||
        (int)normalized_mode > (int)KEYER_KEY_IN_SK_R) {
        normalized_mode = KEYER_KEY_IN_PADDLE;
    }

    if (normalized_volume != s_system_config.volume ||
        normalized_tone_hz != s_system_config.tone_hz ||
        normalized_wpm != s_system_config.key_in_wpm ||
        normalized_mode != s_system_config.key_in_mode ||
        normalized_gps_baud != s_system_config.gps_baud ||
        strcmp(normalized_date, s_system_config.date) != 0 ||
        strcmp(normalized_time, s_system_config.time) != 0) {
        if (changed != NULL) {
            *changed = true;
        }
    }

    s_system_config.volume = normalized_volume;
    s_system_config.tone_hz = normalized_tone_hz;
    s_system_config.key_in_wpm = normalized_wpm;
    s_system_config.key_in_mode = normalized_mode;
    s_system_config.gps_baud = normalized_gps_baud;
    snprintf(s_system_config.date, sizeof(s_system_config.date), "%s", normalized_date);
    snprintf(s_system_config.time, sizeof(s_system_config.time), "%s", normalized_time);
}

static void storage_normalize_keyer_config(bool *changed)
{
    keyer_config_t before = s_keyer_config;

    if ((int)s_keyer_config.key_out_mode < (int)KEYER_KEY_OUT_PADDLE ||
        (int)s_keyer_config.key_out_mode > (int)KEYER_KEY_OUT_OFF) {
        s_keyer_config.key_out_mode = KEYER_KEY_OUT_SK;
    }

    if ((int)s_keyer_config.paddle_mode < (int)KEYER_PADDLE_IAMBIC_A ||
        (int)s_keyer_config.paddle_mode > (int)KEYER_PADDLE_BUG) {
        s_keyer_config.paddle_mode = KEYER_PADDLE_IAMBIC_A;
    }

    s_keyer_config.sk_wpm =
        storage_clamp_u8(s_keyer_config.sk_wpm,
                         STORAGE_KEYER_SK_WPM_MIN,
                         STORAGE_KEYER_SK_WPM_MAX);
    s_keyer_config.tx_delay_s =
        storage_clamp_u8(s_keyer_config.tx_delay_s,
                         STORAGE_KEYER_TX_DELAY_MIN,
                         STORAGE_KEYER_TX_DELAY_MAX);
    s_keyer_config.repeat_interval_s =
        storage_clamp_u8(s_keyer_config.repeat_interval_s,
                         STORAGE_KEYER_REPEAT_MIN,
                         STORAGE_KEYER_REPEAT_MAX);
    s_keyer_config.tune_timeout_s =
        storage_clamp_u8(s_keyer_config.tune_timeout_s,
                         STORAGE_KEYER_TUNE_TIMEOUT_MIN,
                         STORAGE_KEYER_TUNE_TIMEOUT_MAX);

    for (uint8_t i = 0U; i < KEYER_MESSAGE_COUNT; ++i) {
        s_keyer_config.message[i][KEYER_MESSAGE_MAX_LEN] = '\0';
    }
    s_keyer_config.mycall[KEYER_MYCALL_MAX_LEN] = '\0';
    storage_copy_keyer_mycall(s_keyer_config.mycall, s_keyer_config.mycall);
    if (s_keyer_config.mycall[0] == '\0') {
        storage_copy_keyer_mycall(s_keyer_config.mycall, STORAGE_KEYER_DEFAULT_MYCALL);
    }

    if (strcmp(s_keyer_config.message[0], STORAGE_KEYER_PREVIOUS_DEFAULT_M1) == 0) {
        storage_copy_keyer_message(s_keyer_config.message[0], STORAGE_KEYER_DEFAULT_M1);
    }
    if (strcmp(s_keyer_config.message[1], STORAGE_KEYER_OLD_DEFAULT_M2) == 0 ||
        strcmp(s_keyer_config.message[1], STORAGE_KEYER_PREVIOUS_DEFAULT_M2) == 0) {
        storage_copy_keyer_message(s_keyer_config.message[1], STORAGE_KEYER_DEFAULT_M2);
    }
    if (strcmp(s_keyer_config.message[2], STORAGE_KEYER_PREVIOUS_DEFAULT_M3) == 0) {
        storage_copy_keyer_message(s_keyer_config.message[2], STORAGE_KEYER_DEFAULT_M3);
    }
    if (strcmp(s_keyer_config.message[3], STORAGE_KEYER_PREVIOUS_DEFAULT_M4) == 0) {
        storage_copy_keyer_message(s_keyer_config.message[3], STORAGE_KEYER_DEFAULT_M4);
    }
    if (strcmp(s_keyer_config.message[4], STORAGE_KEYER_OLD_DEFAULT_M5) == 0 ||
        strcmp(s_keyer_config.message[4], STORAGE_KEYER_PREVIOUS_DEFAULT_M5) == 0) {
        storage_copy_keyer_message(s_keyer_config.message[4], STORAGE_KEYER_DEFAULT_M5);
    }

    if (memcmp(&before, &s_keyer_config, sizeof(before)) != 0 && changed != NULL) {
        *changed = true;
    }
}

static void storage_normalize_lesson_config(bool *changed)
{
    cw_lesson_config_t before = s_lesson_config;

    s_lesson_config.lesson =
        storage_clamp_u8(s_lesson_config.lesson, STORAGE_LESSON_MIN, STORAGE_LESSON_MAX);
    s_lesson_config.duration_min =
        storage_clamp_u8(s_lesson_config.duration_min,
                         STORAGE_LESSON_DURATION_MIN,
                         STORAGE_LESSON_DURATION_MAX);
    s_lesson_config.code_wpm =
        storage_clamp_u8(s_lesson_config.code_wpm, STORAGE_LESSON_WPM_MIN, STORAGE_LESSON_WPM_MAX);
    s_lesson_config.effective_wpm =
        storage_clamp_u8(s_lesson_config.effective_wpm,
                         STORAGE_LESSON_WPM_MIN,
                         STORAGE_LESSON_WPM_MAX);

    if (s_lesson_config.effective_wpm > s_lesson_config.code_wpm) {
        s_lesson_config.effective_wpm = s_lesson_config.code_wpm;
    }

    if (s_lesson_config.group_len != 0U) {
        s_lesson_config.group_len =
            storage_clamp_u8(s_lesson_config.group_len,
                             STORAGE_LESSON_GROUP_MIN,
                             STORAGE_LESSON_GROUP_MAX);
    }

    if (memcmp(&before, &s_lesson_config, sizeof(before)) != 0 && changed != NULL) {
        *changed = true;
    }
}

static void storage_normalize_word_config(bool *changed)
{
    cw_word_config_t before = s_word_config;

    s_word_config.start_wpm =
        storage_clamp_u8(s_word_config.start_wpm, STORAGE_WORD_WPM_MIN, STORAGE_WORD_WPM_MAX);
    s_word_config.max_wpm =
        storage_clamp_u8(s_word_config.max_wpm, STORAGE_WORD_WPM_MIN, STORAGE_WORD_WPM_MAX);
    s_word_config.min_char_wpm =
        storage_clamp_u8(s_word_config.min_char_wpm, STORAGE_WORD_WPM_MIN, STORAGE_WORD_WPM_MAX);
    s_word_config.lesson =
        storage_clamp_u8(s_word_config.lesson, STORAGE_WORD_LESSON_MIN, STORAGE_WORD_LESSON_MAX);
    s_word_config.max_word_len =
        storage_clamp_u8(s_word_config.max_word_len,
                         STORAGE_WORD_MAX_LEN_MIN,
                         STORAGE_WORD_MAX_LEN_MAX);
    s_word_config.delay_s =
        storage_clamp_u8(s_word_config.delay_s, STORAGE_DELAY_S_MIN, STORAGE_DELAY_S_MAX);

    if (s_word_config.start_wpm > s_word_config.max_wpm) {
        s_word_config.start_wpm = s_word_config.max_wpm;
    }

    if (memcmp(&before, &s_word_config, sizeof(before)) != 0 && changed != NULL) {
        *changed = true;
    }
}

static void storage_normalize_callsign_config(bool *changed)
{
    cw_callsign_config_t before = s_callsign_config;

    s_callsign_config.max_wpm =
        storage_clamp_u8(s_callsign_config.max_wpm,
                         STORAGE_CALLSIGN_WPM_MIN,
                         STORAGE_CALLSIGN_WPM_MAX);
    s_callsign_config.start_wpm =
        storage_clamp_u8(s_callsign_config.start_wpm,
                         STORAGE_CALLSIGN_WPM_MIN,
                         STORAGE_CALLSIGN_WPM_MAX);
    s_callsign_config.min_char_wpm =
        storage_clamp_u8(s_callsign_config.min_char_wpm,
                         STORAGE_CALLSIGN_WPM_MIN,
                         STORAGE_CALLSIGN_WPM_MAX);
    s_callsign_config.delay_s =
        storage_clamp_u8(s_callsign_config.delay_s, STORAGE_DELAY_S_MIN, STORAGE_DELAY_S_MAX);

    if (s_callsign_config.start_wpm > s_callsign_config.max_wpm) {
        s_callsign_config.start_wpm = s_callsign_config.max_wpm;
    }

    if (memcmp(&before, &s_callsign_config, sizeof(before)) != 0 && changed != NULL) {
        *changed = true;
    }
}

static void storage_normalize_plaintext_config(bool *changed)
{
    cw_plaintext_config_t before = s_plaintext_config;

    s_plaintext_config.code_wpm =
        storage_clamp_u8(s_plaintext_config.code_wpm,
                         STORAGE_PLAINTEXT_WPM_MIN,
                         STORAGE_PLAINTEXT_WPM_MAX);
    s_plaintext_config.effective_wpm =
        storage_clamp_u8(s_plaintext_config.effective_wpm,
                         STORAGE_PLAINTEXT_WPM_MIN,
                         STORAGE_PLAINTEXT_WPM_MAX);

    if (s_plaintext_config.effective_wpm > s_plaintext_config.code_wpm) {
        s_plaintext_config.effective_wpm = s_plaintext_config.code_wpm;
    }

    if (memcmp(&before, &s_plaintext_config, sizeof(before)) != 0 && changed != NULL) {
        *changed = true;
    }
}

static void storage_normalize_all_settings(bool *changed)
{
    storage_normalize_system_config(changed);
    storage_normalize_keyer_config(changed);
    storage_normalize_lesson_config(changed);
    storage_normalize_word_config(changed);
    storage_normalize_callsign_config(changed);
    storage_normalize_plaintext_config(changed);
}

static storage_setting_section_t storage_parse_section_name(const char *name)
{
    if (storage_str_equal_ignore_case(name, "system")) {
        return STORAGE_SETTING_SECTION_SYSTEM;
    }
    if (storage_str_equal_ignore_case(name, "keyer")) {
        return STORAGE_SETTING_SECTION_KEYER;
    }
    if (storage_str_equal_ignore_case(name, "lessons")) {
        return STORAGE_SETTING_SECTION_LESSONS;
    }
    if (storage_str_equal_ignore_case(name, "words")) {
        return STORAGE_SETTING_SECTION_WORDS;
    }
    if (storage_str_equal_ignore_case(name, "calls")) {
        return STORAGE_SETTING_SECTION_CALLS;
    }
    if (storage_str_equal_ignore_case(name, "plain")) {
        return STORAGE_SETTING_SECTION_PLAIN;
    }

    return STORAGE_SETTING_SECTION_UNKNOWN;
}

static void storage_mark_section_seen(storage_setting_section_t section, uint32_t *seen_sections)
{
    if (seen_sections == NULL) {
        return;
    }

    switch (section) {
    case STORAGE_SETTING_SECTION_SYSTEM:
        *seen_sections |= STORAGE_SECTION_SYSTEM;
        break;
    case STORAGE_SETTING_SECTION_KEYER:
        *seen_sections |= STORAGE_SECTION_KEYER;
        break;
    case STORAGE_SETTING_SECTION_LESSONS:
        *seen_sections |= STORAGE_SECTION_LESSONS;
        break;
    case STORAGE_SETTING_SECTION_WORDS:
        *seen_sections |= STORAGE_SECTION_WORDS;
        break;
    case STORAGE_SETTING_SECTION_CALLS:
        *seen_sections |= STORAGE_SECTION_CALLS;
        break;
    case STORAGE_SETTING_SECTION_PLAIN:
        *seen_sections |= STORAGE_SECTION_PLAIN;
        break;
    case STORAGE_SETTING_SECTION_NONE:
    case STORAGE_SETTING_SECTION_UNKNOWN:
    default:
        break;
    }
}

static bool storage_apply_u8_setting(const char *value,
                                     uint8_t min_value,
                                     uint8_t max_value,
                                     uint8_t *target,
                                     bool *changed)
{
    uint32_t parsed;

    if (target == NULL || !storage_parse_u32(value, &parsed)) {
        if (changed != NULL) {
            *changed = true;
        }
        return false;
    }

    *target = storage_clamp_u32_to_u8(parsed, min_value, max_value, changed);
    return true;
}

static bool storage_apply_u16_setting(const char *value, uint16_t *target, bool *changed)
{
    uint32_t parsed;

    if (target == NULL || !storage_parse_u32(value, &parsed) || parsed > UINT16_MAX) {
        if (changed != NULL) {
            *changed = true;
        }
        return false;
    }

    *target = (uint16_t)parsed;
    return true;
}

static bool storage_apply_gps_baud_setting(const char *value, bool *changed)
{
    uint32_t parsed;
    int normalized;

    if (!storage_parse_u32(value, &parsed) || parsed > (uint32_t)INT_MAX) {
        if (changed != NULL) {
            *changed = true;
        }
        return false;
    }

    normalized = storage_normalize_gps_baud((int)parsed);
    if (parsed != (uint32_t)normalized && changed != NULL) {
        *changed = true;
    }
    s_system_config.gps_baud = normalized;
    return true;
}

static bool storage_apply_lesson_group_len(const char *value, bool *changed)
{
    uint32_t parsed;

    if (!storage_parse_u32(value, &parsed)) {
        if (changed != NULL) {
            *changed = true;
        }
        return false;
    }

    if (parsed == 0U) {
        s_lesson_config.group_len = 0U;
        return true;
    }

    s_lesson_config.group_len =
        storage_clamp_u32_to_u8(parsed, STORAGE_LESSON_GROUP_MIN, STORAGE_LESSON_GROUP_MAX, changed);
    return true;
}

static void storage_apply_setting(storage_setting_section_t section,
                                  const char *key,
                                  const char *value,
                                  uint64_t *seen_keys,
                                  bool *changed)
{
    bool usb_drive_requested = false;
    keyer_key_in_mode_t key_in_mode;
    keyer_key_out_mode_t key_out_mode;
    keyer_paddle_mode_t paddle_mode;

    if (key == NULL || value == NULL || seen_keys == NULL) {
        return;
    }

    switch (section) {
    case STORAGE_SETTING_SECTION_SYSTEM:
        if (storage_str_equal_ignore_case(key, "volume")) {
            *seen_keys |= STORAGE_KEY_SYSTEM_VOLUME;
            (void)storage_apply_u8_setting(value,
                                           STORAGE_VOLUME_MIN,
                                           STORAGE_VOLUME_MAX,
                                           &s_system_config.volume,
                                           changed);
        } else if (storage_str_equal_ignore_case(key, "tone_hz")) {
            *seen_keys |= STORAGE_KEY_SYSTEM_TONE_HZ;
            (void)storage_apply_u16_setting(value, &s_system_config.tone_hz, changed);
        } else if (storage_str_equal_ignore_case(key, "key_in")) {
            *seen_keys |= STORAGE_KEY_SYSTEM_KEY_IN;
            if (storage_parse_key_in_mode(value, &key_in_mode)) {
                s_system_config.key_in_mode = key_in_mode;
            } else if (changed != NULL) {
                *changed = true;
            }
        } else if (storage_str_equal_ignore_case(key, "key_in_wpm")) {
            *seen_keys |= STORAGE_KEY_SYSTEM_KEY_IN_WPM;
            (void)storage_apply_u8_setting(value,
                                           STORAGE_KEY_WPM_MIN,
                                           STORAGE_KEY_WPM_MAX,
                                           &s_system_config.key_in_wpm,
                                           changed);
        } else if (storage_str_equal_ignore_case(key, "gps_baud")) {
            *seen_keys |= STORAGE_KEY_SYSTEM_GPS_BAUD;
            (void)storage_apply_gps_baud_setting(value, changed);
        } else if (storage_str_equal_ignore_case(key, "usb_drive")) {
            *seen_keys |= STORAGE_KEY_SYSTEM_USB_DRIVE;
            if (!storage_parse_bool(value, &usb_drive_requested) || usb_drive_requested) {
                if (changed != NULL) {
                    *changed = true;
                }
            }
        } else if (storage_str_equal_ignore_case(key, "date")) {
            *seen_keys |= STORAGE_KEY_SYSTEM_DATE;
            (void)storage_apply_datetime_string(value,
                                                s_system_config.date,
                                                sizeof(s_system_config.date),
                                                true,
                                                changed);
        } else if (storage_str_equal_ignore_case(key, "time")) {
            *seen_keys |= STORAGE_KEY_SYSTEM_TIME;
            (void)storage_apply_datetime_string(value,
                                                s_system_config.time,
                                                sizeof(s_system_config.time),
                                                false,
                                                changed);
        }
        break;

    case STORAGE_SETTING_SECTION_KEYER:
        if (storage_str_equal_ignore_case(key, "key_out")) {
            *seen_keys |= STORAGE_KEY_KEYER_KEY_OUT;
            if (storage_parse_key_out_mode(value, &key_out_mode)) {
                s_keyer_config.key_out_mode = key_out_mode;
            } else if (changed != NULL) {
                *changed = true;
            }
        } else if (storage_str_equal_ignore_case(key, "paddle")) {
            *seen_keys |= STORAGE_KEY_KEYER_PADDLE;
            if (storage_parse_paddle_mode(value, &paddle_mode)) {
                s_keyer_config.paddle_mode = paddle_mode;
            } else if (changed != NULL) {
                *changed = true;
            }
        } else if (storage_str_equal_ignore_case(key, "sk_wpm")) {
            *seen_keys |= STORAGE_KEY_KEYER_SK_WPM;
            (void)storage_apply_u8_setting(value,
                                           STORAGE_KEYER_SK_WPM_MIN,
                                           STORAGE_KEYER_SK_WPM_MAX,
                                           &s_keyer_config.sk_wpm,
                                           changed);
        } else if (storage_str_equal_ignore_case(key, "tx_delay_s")) {
            *seen_keys |= STORAGE_KEY_KEYER_TX_DELAY;
            (void)storage_apply_u8_setting(value,
                                           STORAGE_KEYER_TX_DELAY_MIN,
                                           STORAGE_KEYER_TX_DELAY_MAX,
                                           &s_keyer_config.tx_delay_s,
                                           changed);
        } else if (storage_str_equal_ignore_case(key, "repeat_interval_s")) {
            *seen_keys |= STORAGE_KEY_KEYER_REPEAT;
            (void)storage_apply_u8_setting(value,
                                           STORAGE_KEYER_REPEAT_MIN,
                                           STORAGE_KEYER_REPEAT_MAX,
                                           &s_keyer_config.repeat_interval_s,
                                           changed);
        } else if (storage_str_equal_ignore_case(key, "tune_timeout_s")) {
            *seen_keys |= STORAGE_KEY_KEYER_TUNE_TIMEOUT;
            (void)storage_apply_u8_setting(value,
                                           STORAGE_KEYER_TUNE_TIMEOUT_MIN,
                                           STORAGE_KEYER_TUNE_TIMEOUT_MAX,
                                           &s_keyer_config.tune_timeout_s,
                                           changed);
        } else if (storage_str_equal_ignore_case(key, "mycall")) {
            *seen_keys |= STORAGE_KEY_KEYER_MYCALL;
            storage_copy_keyer_mycall(s_keyer_config.mycall, value);
        } else if (storage_str_equal_ignore_case(key, "m1")) {
            *seen_keys |= STORAGE_KEY_KEYER_M1;
            storage_copy_keyer_message(s_keyer_config.message[0], value);
        } else if (storage_str_equal_ignore_case(key, "m2")) {
            *seen_keys |= STORAGE_KEY_KEYER_M2;
            storage_copy_keyer_message(s_keyer_config.message[1], value);
        } else if (storage_str_equal_ignore_case(key, "m3")) {
            *seen_keys |= STORAGE_KEY_KEYER_M3;
            storage_copy_keyer_message(s_keyer_config.message[2], value);
        } else if (storage_str_equal_ignore_case(key, "m4")) {
            *seen_keys |= STORAGE_KEY_KEYER_M4;
            storage_copy_keyer_message(s_keyer_config.message[3], value);
        } else if (storage_str_equal_ignore_case(key, "m5")) {
            *seen_keys |= STORAGE_KEY_KEYER_M5;
            storage_copy_keyer_message(s_keyer_config.message[4], value);
        }
        break;

    case STORAGE_SETTING_SECTION_LESSONS:
        if (storage_str_equal_ignore_case(key, "lesson")) {
            *seen_keys |= STORAGE_KEY_LESSON_LESSON;
            (void)storage_apply_u8_setting(value,
                                           STORAGE_LESSON_MIN,
                                           STORAGE_LESSON_MAX,
                                           &s_lesson_config.lesson,
                                           changed);
        } else if (storage_str_equal_ignore_case(key, "duration_min")) {
            *seen_keys |= STORAGE_KEY_LESSON_DURATION;
            (void)storage_apply_u8_setting(value,
                                           STORAGE_LESSON_DURATION_MIN,
                                           STORAGE_LESSON_DURATION_MAX,
                                           &s_lesson_config.duration_min,
                                           changed);
        } else if (storage_str_equal_ignore_case(key, "code_wpm")) {
            *seen_keys |= STORAGE_KEY_LESSON_CODE_WPM;
            (void)storage_apply_u8_setting(value,
                                           STORAGE_LESSON_WPM_MIN,
                                           STORAGE_LESSON_WPM_MAX,
                                           &s_lesson_config.code_wpm,
                                           changed);
        } else if (storage_str_equal_ignore_case(key, "effective_wpm")) {
            *seen_keys |= STORAGE_KEY_LESSON_EFFECTIVE_WPM;
            (void)storage_apply_u8_setting(value,
                                           STORAGE_LESSON_WPM_MIN,
                                           STORAGE_LESSON_WPM_MAX,
                                           &s_lesson_config.effective_wpm,
                                           changed);
        } else if (storage_str_equal_ignore_case(key, "group_len")) {
            *seen_keys |= STORAGE_KEY_LESSON_GROUP_LEN;
            (void)storage_apply_lesson_group_len(value, changed);
        }
        break;

    case STORAGE_SETTING_SECTION_WORDS:
        if (storage_str_equal_ignore_case(key, "speed")) {
            *seen_keys |= STORAGE_KEY_WORD_SPEED;
            (void)storage_apply_u8_setting(value,
                                           STORAGE_WORD_WPM_MIN,
                                           STORAGE_WORD_WPM_MAX,
                                           &s_word_config.start_wpm,
                                           changed);
        } else if (storage_str_equal_ignore_case(key, "min_char_wpm")) {
            *seen_keys |= STORAGE_KEY_WORD_MIN_CHAR_WPM;
            (void)storage_apply_u8_setting(value,
                                           STORAGE_WORD_WPM_MIN,
                                           STORAGE_WORD_WPM_MAX,
                                           &s_word_config.min_char_wpm,
                                           changed);
        } else if (storage_str_equal_ignore_case(key, "lesson")) {
            *seen_keys |= STORAGE_KEY_WORD_LESSON;
            (void)storage_apply_u8_setting(value,
                                           STORAGE_WORD_LESSON_MIN,
                                           STORAGE_WORD_LESSON_MAX,
                                           &s_word_config.lesson,
                                           changed);
        } else if (storage_str_equal_ignore_case(key, "max_len")) {
            *seen_keys |= STORAGE_KEY_WORD_MAX_LEN;
            (void)storage_apply_u8_setting(value,
                                           STORAGE_WORD_MAX_LEN_MIN,
                                           STORAGE_WORD_MAX_LEN_MAX,
                                           &s_word_config.max_word_len,
                                           changed);
        } else if (storage_str_equal_ignore_case(key, "max_wpm")) {
            *seen_keys |= STORAGE_KEY_WORD_MAX_WPM;
            (void)storage_apply_u8_setting(value,
                                           STORAGE_WORD_WPM_MIN,
                                           STORAGE_WORD_WPM_MAX,
                                           &s_word_config.max_wpm,
                                           changed);
        } else if (storage_str_equal_ignore_case(key, "delay_s")) {
            *seen_keys |= STORAGE_KEY_WORD_DELAY_S;
            (void)storage_apply_u8_setting(value,
                                           STORAGE_DELAY_S_MIN,
                                           STORAGE_DELAY_S_MAX,
                                           &s_word_config.delay_s,
                                           changed);
        }
        break;

    case STORAGE_SETTING_SECTION_CALLS:
        if (storage_str_equal_ignore_case(key, "speed")) {
            *seen_keys |= STORAGE_KEY_CALLSIGN_SPEED;
            (void)storage_apply_u8_setting(value,
                                           STORAGE_CALLSIGN_WPM_MIN,
                                           STORAGE_CALLSIGN_WPM_MAX,
                                           &s_callsign_config.start_wpm,
                                           changed);
        } else if (storage_str_equal_ignore_case(key, "min_char_wpm")) {
            *seen_keys |= STORAGE_KEY_CALLSIGN_MIN_CHAR_WPM;
            (void)storage_apply_u8_setting(value,
                                           STORAGE_CALLSIGN_WPM_MIN,
                                           STORAGE_CALLSIGN_WPM_MAX,
                                           &s_callsign_config.min_char_wpm,
                                           changed);
        } else if (storage_str_equal_ignore_case(key, "max_wpm")) {
            *seen_keys |= STORAGE_KEY_CALLSIGN_MAX_WPM;
            (void)storage_apply_u8_setting(value,
                                           STORAGE_CALLSIGN_WPM_MIN,
                                           STORAGE_CALLSIGN_WPM_MAX,
                                           &s_callsign_config.max_wpm,
                                           changed);
        } else if (storage_str_equal_ignore_case(key, "delay_s")) {
            *seen_keys |= STORAGE_KEY_CALLSIGN_DELAY_S;
            (void)storage_apply_u8_setting(value,
                                           STORAGE_DELAY_S_MIN,
                                           STORAGE_DELAY_S_MAX,
                                           &s_callsign_config.delay_s,
                                           changed);
        }
        break;

    case STORAGE_SETTING_SECTION_PLAIN:
        if (storage_str_equal_ignore_case(key, "code_wpm")) {
            *seen_keys |= STORAGE_KEY_PLAINTEXT_CODE_WPM;
            (void)storage_apply_u8_setting(value,
                                           STORAGE_PLAINTEXT_WPM_MIN,
                                           STORAGE_PLAINTEXT_WPM_MAX,
                                           &s_plaintext_config.code_wpm,
                                           changed);
        } else if (storage_str_equal_ignore_case(key, "effective_wpm")) {
            *seen_keys |= STORAGE_KEY_PLAINTEXT_EFFECTIVE_WPM;
            (void)storage_apply_u8_setting(value,
                                           STORAGE_PLAINTEXT_WPM_MIN,
                                           STORAGE_PLAINTEXT_WPM_MAX,
                                           &s_plaintext_config.effective_wpm,
                                           changed);
        }
        break;

    case STORAGE_SETTING_SECTION_NONE:
    case STORAGE_SETTING_SECTION_UNKNOWN:
    default:
        break;
    }
}

static bool storage_remove_file_if_exists(const char *path, const char *description)
{
    errno = 0;
    if (remove(path) == 0) {
        return true;
    }

    if (errno == ENOENT) {
        return true;
    }

    ESP_LOGE(TAG, "remove %s failed for %s: errno=%d", path, description, errno);
    return false;
}

static void storage_cleanup_tmp_settings_file(void)
{
    if (!storage_remove_file_if_exists(STORAGE_SETTINGS_TMP_PATH, "temporary settings cleanup")) {
        ESP_LOGW(TAG, "temporary settings file may remain at %s", STORAGE_SETTINGS_TMP_PATH);
    }
}

static bool storage_write_settings_file(void)
{
    FILE *file;
    int close_result;
    int write_result;

    if (!storage_firmware_can_access_fatfs("settings save")) {
        return false;
    }

    if (!storage_remove_file_if_exists(STORAGE_SETTINGS_TMP_PATH, "stale temporary settings")) {
        return false;
    }

    file = fopen(STORAGE_SETTINGS_TMP_PATH, "w");
    if (file == NULL) {
        ESP_LOGE(TAG, "open %s for write failed", STORAGE_SETTINGS_TMP_PATH);
        return false;
    }

    write_result = fprintf(file,
                           "# Mini-CW setting.txt\n"
                           "# Edit while USB Drive is ON, eject safely, then turn USB Drive OFF or reboot.\n"
                           "\n"
                            "[system]\n"
                            "volume=%u\n"
                            "tone_hz=%u\n"
                            "key_in=%s\n"
                            "key_in_wpm=%u\n"
                            "date=%s\n"
                            "time=%s\n"
                            "gps_baud=%d\n"
                            "usb_drive=off\n"
                           "\n"
                           "[keyer]\n"
                           "key_out=%s\n"
                           "paddle=%s\n"
                           "sk_wpm=%u\n"
                           "tx_delay_s=%u\n"
                           "tune_timeout_s=%u\n"
                           "repeat_interval_s=%u\n"
                           "mycall=%s\n"
                           "m1=%s\n"
                           "m2=%s\n"
                           "m3=%s\n"
                           "m4=%s\n"
                           "m5=%s\n"
                           "\n"
                           "[lessons]\n"
                           "lesson=%u\n"
                           "duration_min=%u\n"
                           "code_wpm=%u\n"
                           "effective_wpm=%u\n"
                           "group_len=%u\n"
                           "\n"
                           "[words]\n"
                           "speed=%u\n"
                           "min_char_wpm=%u\n"
                           "lesson=%u\n"
                           "max_len=%u\n"
                           "max_wpm=%u\n"
                           "delay_s=%u\n"
                           "\n"
                           "[calls]\n"
                           "speed=%u\n"
                           "min_char_wpm=%u\n"
                           "max_wpm=%u\n"
                           "delay_s=%u\n"
                           "\n"
                           "[plain]\n"
                           "code_wpm=%u\n"
                           "effective_wpm=%u\n",
                           (unsigned)s_system_config.volume,
                           (unsigned)s_system_config.tone_hz,
                           storage_key_in_mode_label(s_system_config.key_in_mode),
                           (unsigned)s_system_config.key_in_wpm,
                           s_system_config.date,
                           s_system_config.time,
                           s_system_config.gps_baud,
                           storage_key_out_mode_label(s_keyer_config.key_out_mode),
                           keyer_service_paddle_mode_label(s_keyer_config.paddle_mode),
                           (unsigned)s_keyer_config.sk_wpm,
                           (unsigned)s_keyer_config.tx_delay_s,
                           (unsigned)s_keyer_config.tune_timeout_s,
                           (unsigned)s_keyer_config.repeat_interval_s,
                           s_keyer_config.mycall,
                           s_keyer_config.message[0],
                           s_keyer_config.message[1],
                           s_keyer_config.message[2],
                           s_keyer_config.message[3],
                           s_keyer_config.message[4],
                           (unsigned)s_lesson_config.lesson,
                           (unsigned)s_lesson_config.duration_min,
                           (unsigned)s_lesson_config.code_wpm,
                           (unsigned)s_lesson_config.effective_wpm,
                           (unsigned)s_lesson_config.group_len,
                           (unsigned)s_word_config.start_wpm,
                           (unsigned)s_word_config.min_char_wpm,
                           (unsigned)s_word_config.lesson,
                           (unsigned)s_word_config.max_word_len,
                           (unsigned)s_word_config.max_wpm,
                           (unsigned)s_word_config.delay_s,
                           (unsigned)s_callsign_config.start_wpm,
                           (unsigned)s_callsign_config.min_char_wpm,
                           (unsigned)s_callsign_config.max_wpm,
                           (unsigned)s_callsign_config.delay_s,
                           (unsigned)s_plaintext_config.code_wpm,
                           (unsigned)s_plaintext_config.effective_wpm);
    if (write_result < 0 || ferror(file) != 0) {
        ESP_LOGE(TAG, "write %s failed", STORAGE_SETTINGS_TMP_PATH);
        (void)fclose(file);
        storage_cleanup_tmp_settings_file();
        return false;
    }

    if (fflush(file) != 0) {
        ESP_LOGE(TAG, "flush %s failed: errno=%d", STORAGE_SETTINGS_TMP_PATH, errno);
        (void)fclose(file);
        storage_cleanup_tmp_settings_file();
        return false;
    }

    close_result = fclose(file);
    if (close_result != 0) {
        ESP_LOGE(TAG, "close %s after write failed: errno=%d", STORAGE_SETTINGS_TMP_PATH, errno);
        storage_cleanup_tmp_settings_file();
        return false;
    }

    if (!storage_remove_file_if_exists(STORAGE_SETTINGS_PATH, "settings replacement")) {
        storage_cleanup_tmp_settings_file();
        return false;
    }

    if (rename(STORAGE_SETTINGS_TMP_PATH, STORAGE_SETTINGS_PATH) != 0) {
        ESP_LOGE(TAG,
                 "rename %s to %s failed: errno=%d",
                 STORAGE_SETTINGS_TMP_PATH,
                 STORAGE_SETTINGS_PATH,
                 errno);
        storage_cleanup_tmp_settings_file();
        return false;
    }

    ESP_LOGI(TAG, "settings saved to %s", STORAGE_SETTINGS_PATH);
    return true;
}

static bool storage_write_default_qsocalls_file(void)
{
    FILE *file;
    int close_result;

    file = fopen(STORAGE_QSOCALLS_PATH, "w");
    if (file == NULL) {
        ESP_LOGW(TAG, "open %s for default write failed: errno=%d", STORAGE_QSOCALLS_PATH, errno);
        return false;
    }

    if (fputs(STORAGE_QSOCALLS_HEADER, file) == EOF || ferror(file) != 0) {
        ESP_LOGW(TAG, "write %s defaults failed", STORAGE_QSOCALLS_PATH);
        (void)fclose(file);
        return false;
    }

    if (fflush(file) != 0) {
        ESP_LOGW(TAG, "flush %s defaults failed: errno=%d", STORAGE_QSOCALLS_PATH, errno);
        (void)fclose(file);
        return false;
    }

    close_result = fclose(file);
    if (close_result != 0) {
        ESP_LOGW(TAG, "close %s defaults failed: errno=%d", STORAGE_QSOCALLS_PATH, errno);
        return false;
    }

    ESP_LOGI(TAG, "%s missing; created default header", STORAGE_QSOCALLS_PATH);
    return true;
}

void storage_service_init(void)
{
    const esp_partition_t *partition;
    esp_err_t err;

    partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                         ESP_PARTITION_SUBTYPE_DATA_FAT,
                                         STORAGE_FATFS_LABEL);
    if (partition == NULL) {
        ESP_LOGE(TAG, "FATFS partition '%s' not found", STORAGE_FATFS_LABEL);
        return;
    }

    err = wl_mount(partition, &s_wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wear levelling mount failed: %s", esp_err_to_name(err));
        s_wl_handle = WL_INVALID_HANDLE;
        return;
    }

    const tinyusb_msc_spiflash_config_t config_spi = {
        .wl_handle = s_wl_handle,
        .callback_mount_changed = storage_mount_changed_cb,
        .mount_config = {
            .format_if_mount_failed = true,
            .max_files = STORAGE_FATFS_MAX_FILES,
            .allocation_unit_size = STORAGE_FATFS_ALLOC_UNIT,
        },
    };

    err = tinyusb_msc_storage_init_spiflash(&config_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB MSC storage init failed: %s", esp_err_to_name(err));
        (void)wl_unmount(s_wl_handle);
        s_wl_handle = WL_INVALID_HANDLE;
        return;
    }

    s_storage_ready = true;
    s_usb_drive_enabled = false;
    s_tinyusb_installed = false;

    if (storage_mount_fatfs() != ESP_OK) {
        ESP_LOGW(TAG, "FATFS unavailable to firmware");
    }

    ESP_LOGI(TAG, "initialized storage owner: FATFS partition ready, USB Drive OFF");
}

bool storage_profile_load(void)
{
    FILE *file;
    char line[STORAGE_SETTINGS_LINE_MAX];
    storage_setting_section_t current_section = STORAGE_SETTING_SECTION_NONE;
    uint64_t seen_keys = 0;
    uint32_t seen_sections = 0;
    bool needs_rewrite = false;

    if (!storage_firmware_can_access_fatfs("profile load")) {
        return false;
    }

    storage_settings_set_defaults();

    file = fopen(STORAGE_SETTINGS_PATH, "r");
    if (file == NULL) {
        s_settings_loaded = true;
        ESP_LOGI(TAG, "%s missing; creating defaults", STORAGE_SETTINGS_PATH);
        return storage_write_settings_file();
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char *trimmed;
        char *equals;

        storage_strip_inline_comment(line);
        trimmed = storage_trim(line);
        if (trimmed == NULL || *trimmed == '\0') {
            continue;
        }

        if (*trimmed == '[') {
            char *end = strchr(trimmed, ']');
            if (end == NULL) {
                needs_rewrite = true;
                current_section = STORAGE_SETTING_SECTION_UNKNOWN;
                continue;
            }

            *end = '\0';
            current_section = storage_parse_section_name(storage_trim(trimmed + 1));
            storage_mark_section_seen(current_section, &seen_sections);
            continue;
        }

        equals = strchr(trimmed, '=');
        if (equals == NULL) {
            needs_rewrite = true;
            continue;
        }

        *equals = '\0';
        storage_apply_setting(current_section,
                              storage_trim(trimmed),
                              storage_trim(equals + 1),
                              &seen_keys,
                              &needs_rewrite);
    }

    if (ferror(file) != 0) {
        ESP_LOGE(TAG, "read %s failed", STORAGE_SETTINGS_PATH);
        (void)fclose(file);
        return false;
    }

    (void)fclose(file);

    if ((seen_keys & STORAGE_KEY_KEYER_SK_WPM) == 0U) {
        s_keyer_config.sk_wpm = s_system_config.key_in_wpm;
        needs_rewrite = true;
    }

    storage_normalize_all_settings(&needs_rewrite);
    if ((seen_keys & STORAGE_EXPECTED_KEYS) != STORAGE_EXPECTED_KEYS ||
        (seen_sections & STORAGE_EXPECTED_SECTIONS) != STORAGE_EXPECTED_SECTIONS) {
        needs_rewrite = true;
    }

    s_settings_loaded = true;

    if (needs_rewrite && !storage_write_settings_file()) {
        ESP_LOGW(TAG, "settings loaded but canonical rewrite failed");
    }

    ESP_LOGI(TAG, "settings loaded from %s", STORAGE_SETTINGS_PATH);
    return true;
}

bool storage_profile_save(void)
{
    if (!storage_firmware_can_access_fatfs("profile save")) {
        return false;
    }

    if (!s_settings_loaded) {
        storage_settings_set_defaults();
        s_settings_loaded = true;
    }

    storage_normalize_all_settings(NULL);
    return storage_write_settings_file();
}

bool storage_session_log_append(const char *line)
{
    FILE *file;
    bool ok = true;

    if (!storage_firmware_can_access_fatfs("session log")) {
        return false;
    }

    if (line == NULL) {
        return false;
    }

    file = fopen(STORAGE_KEYER_LOG_PATH, "a");
    if (file == NULL) {
        ESP_LOGW(TAG, "open %s for append failed: errno=%d", STORAGE_KEYER_LOG_PATH, errno);
        return false;
    }

    if (fprintf(file, "%s\n", line) < 0) {
        ESP_LOGW(TAG, "write %s failed", STORAGE_KEYER_LOG_PATH);
        ok = false;
    }

    if (fflush(file) != 0) {
        ESP_LOGW(TAG, "flush %s failed: errno=%d", STORAGE_KEYER_LOG_PATH, errno);
        ok = false;
    }

    if (fclose(file) != 0) {
        ESP_LOGW(TAG, "close %s after append failed: errno=%d", STORAGE_KEYER_LOG_PATH, errno);
        ok = false;
    }

    return ok;
}

bool storage_qsocalls_load(keyer_op_entry_t **entries, size_t *count)
{
    FILE *file;
    char line[STORAGE_QSOCALLS_LINE_MAX];
    keyer_op_entry_t *loaded = NULL;
    size_t loaded_count = 0U;
    size_t capacity = 0U;
    bool read_error = false;

    if (entries == NULL || count == NULL) {
        return false;
    }

    *entries = NULL;
    *count = 0U;

    if (!storage_firmware_can_access_fatfs("qsocalls.csv load")) {
        return false;
    }

    errno = 0;
    file = fopen(STORAGE_QSOCALLS_PATH, "r");
    if (file == NULL) {
        if (errno == ENOENT) {
            (void)storage_write_default_qsocalls_file();
        } else {
            ESP_LOGW(TAG, "open %s failed: errno=%d", STORAGE_QSOCALLS_PATH, errno);
        }
        return true;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char *trimmed = storage_trim(line);
        char *comma;
        char *call;
        char *name;
        size_t call_len;
        size_t name_len;

        if (trimmed == NULL || *trimmed == '\0' || *trimmed == '#' || *trimmed == ';') {
            continue;
        }

        comma = strchr(trimmed, ',');
        if (comma == NULL) {
            continue;
        }
        *comma = '\0';
        call = storage_trim(trimmed);
        name = storage_trim(comma + 1);
        if (call == NULL || name == NULL || strchr(name, ',') != NULL) {
            continue;
        }

        if (storage_str_equal_ignore_case(call, "call") &&
            storage_str_equal_ignore_case(name, "name")) {
            continue;
        }

        call_len = strlen(call);
        name_len = strlen(name);
        if (call_len == 0U || call_len > KEYER_OP_CALL_MAX_LEN ||
            name_len == 0U || name_len > KEYER_OP_NAME_MAX_LEN) {
            continue;
        }

        if (loaded_count >= capacity) {
            size_t next_capacity = capacity == 0U ? 64U : capacity * 2U;
            keyer_op_entry_t *next =
                (keyer_op_entry_t *)realloc(loaded, next_capacity * sizeof(*loaded));
            if (next == NULL) {
                ESP_LOGW(TAG,
                         "OP lookup allocation failed after %u rows; using partial table",
                         (unsigned)loaded_count);
                break;
            }
            loaded = next;
            capacity = next_capacity;
        }

        snprintf(loaded[loaded_count].call, sizeof(loaded[loaded_count].call), "%s", call);
        snprintf(loaded[loaded_count].name, sizeof(loaded[loaded_count].name), "%s", name);
        ++loaded_count;
    }

    if (ferror(file) != 0) {
        ESP_LOGW(TAG, "read %s failed; using %u rows", STORAGE_QSOCALLS_PATH, (unsigned)loaded_count);
        read_error = true;
    }

    (void)fclose(file);

    *entries = loaded;
    *count = loaded_count;
    ESP_LOGI(TAG, "loaded %u OP lookup rows from %s", (unsigned)loaded_count, STORAGE_QSOCALLS_PATH);
    return !read_error || loaded_count > 0U;
}

bool storage_system_load_config(storage_system_config_t *config)
{
    if (!storage_firmware_can_access_fatfs("system config load")) {
        return false;
    }

    if (!s_settings_loaded || config == NULL) {
        return false;
    }

    *config = s_system_config;
    return true;
}

bool storage_system_save_config(const storage_system_config_t *config)
{
    if (!storage_firmware_can_access_fatfs("system config save")) {
        return false;
    }

    if (config == NULL) {
        return false;
    }

    if (!s_settings_loaded) {
        storage_settings_set_defaults();
    }

    s_system_config = *config;
    s_settings_loaded = true;
    storage_normalize_system_config(NULL);
    return storage_write_settings_file();
}

bool storage_keyer_load_config(keyer_config_t *config)
{
    if (!storage_firmware_can_access_fatfs("keyer config load")) {
        return false;
    }

    if (!s_settings_loaded || config == NULL) {
        return false;
    }

    *config = s_keyer_config;
    return true;
}

bool storage_keyer_save_config(const keyer_config_t *config)
{
    if (!storage_firmware_can_access_fatfs("keyer config save")) {
        return false;
    }

    if (config == NULL) {
        return false;
    }

    if (!s_settings_loaded) {
        storage_settings_set_defaults();
    }

    s_keyer_config = *config;
    s_settings_loaded = true;
    storage_normalize_keyer_config(NULL);
    return storage_write_settings_file();
}

bool storage_lesson_load(cw_lesson_config_t *config, cw_lesson_result_t *result)
{
    if (!storage_firmware_can_access_fatfs("lesson load")) {
        return false;
    }

    if (!s_settings_loaded || config == NULL) {
        return false;
    }

    *config = s_lesson_config;
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
    }

    return true;
}

bool storage_lesson_save_config(const cw_lesson_config_t *config)
{
    if (!storage_firmware_can_access_fatfs("lesson config save")) {
        return false;
    }

    if (config == NULL) {
        return false;
    }

    if (!s_settings_loaded) {
        storage_settings_set_defaults();
    }

    s_lesson_config = *config;
    s_settings_loaded = true;
    storage_normalize_lesson_config(NULL);
    return storage_write_settings_file();
}

bool storage_lesson_save_result(const cw_lesson_result_t *result)
{
    (void)result;

    if (!storage_firmware_can_access_fatfs("lesson result save")) {
        return false;
    }

    ESP_LOGI(TAG, "lesson result save skipped: persistence disabled");
    return false;
}

bool storage_word_load(cw_word_config_t *config, cw_word_result_t *result)
{
    if (!storage_firmware_can_access_fatfs("word load")) {
        return false;
    }

    if (!s_settings_loaded || config == NULL) {
        return false;
    }

    *config = s_word_config;
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
    }

    return true;
}

bool storage_word_save_config(const cw_word_config_t *config)
{
    if (!storage_firmware_can_access_fatfs("word config save")) {
        return false;
    }

    if (config == NULL) {
        return false;
    }

    if (!s_settings_loaded) {
        storage_settings_set_defaults();
    }

    s_word_config = *config;
    s_settings_loaded = true;
    storage_normalize_word_config(NULL);
    return storage_write_settings_file();
}

bool storage_word_save_result(const cw_word_result_t *result)
{
    (void)result;

    if (!storage_firmware_can_access_fatfs("word result save")) {
        return false;
    }

    ESP_LOGI(TAG, "word result save skipped: persistence disabled");
    return false;
}

bool storage_callsign_load(cw_callsign_config_t *config, cw_callsign_result_t *result)
{
    if (!storage_firmware_can_access_fatfs("callsign load")) {
        return false;
    }

    if (!s_settings_loaded || config == NULL) {
        return false;
    }

    *config = s_callsign_config;
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
    }

    return true;
}

bool storage_callsign_save_config(const cw_callsign_config_t *config)
{
    if (!storage_firmware_can_access_fatfs("callsign config save")) {
        return false;
    }

    if (config == NULL) {
        return false;
    }

    if (!s_settings_loaded) {
        storage_settings_set_defaults();
    }

    s_callsign_config = *config;
    s_settings_loaded = true;
    storage_normalize_callsign_config(NULL);
    return storage_write_settings_file();
}

bool storage_callsign_save_result(const cw_callsign_result_t *result)
{
    (void)result;

    if (!storage_firmware_can_access_fatfs("callsign result save")) {
        return false;
    }

    ESP_LOGI(TAG, "callsign result save skipped: persistence disabled");
    return false;
}

bool storage_plaintext_load(cw_plaintext_config_t *config, cw_plaintext_result_t *result)
{
    if (!storage_firmware_can_access_fatfs("plaintext load")) {
        return false;
    }

    if (!s_settings_loaded || config == NULL) {
        return false;
    }

    *config = s_plaintext_config;
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
    }

    return true;
}

bool storage_plaintext_save_config(const cw_plaintext_config_t *config)
{
    if (!storage_firmware_can_access_fatfs("plaintext config save")) {
        return false;
    }

    if (config == NULL) {
        return false;
    }

    if (!s_settings_loaded) {
        storage_settings_set_defaults();
    }

    s_plaintext_config = *config;
    s_settings_loaded = true;
    storage_normalize_plaintext_config(NULL);
    return storage_write_settings_file();
}

bool storage_plaintext_save_result(const cw_plaintext_result_t *result)
{
    (void)result;

    if (!storage_firmware_can_access_fatfs("plaintext result save")) {
        return false;
    }

    ESP_LOGI(TAG, "plaintext result save skipped: persistence disabled");
    return false;
}

bool storage_fatfs_is_mounted(void)
{
    return s_fatfs_mounted;
}

bool storage_usb_drive_is_enabled(void)
{
    return s_usb_drive_enabled;
}

bool storage_usb_drive_set_enabled(bool enabled)
{
    esp_err_t err;

    if (!s_storage_ready) {
        ESP_LOGW(TAG, "USB Drive change skipped: storage is not ready");
        return false;
    }

    if (enabled == s_usb_drive_enabled) {
        return true;
    }

    if (enabled) {
        err = storage_unmount_fatfs();
        if (err != ESP_OK) {
            return false;
        }

        err = storage_install_tinyusb();
        if (err != ESP_OK) {
            (void)storage_mount_fatfs();
            return false;
        }

        s_usb_drive_enabled = true;
        ESP_LOGI(TAG, "USB Drive ON: PC owns FATFS");
        return true;
    }

    err = storage_uninstall_tinyusb();
    if (err != ESP_OK) {
        return false;
    }

    err = storage_mount_fatfs();
    if (err != ESP_OK) {
        s_usb_drive_enabled = true;
        ESP_LOGE(TAG, "USB Drive remains logically ON until FATFS remount succeeds");
        return false;
    }

    s_usb_drive_enabled = false;
    ESP_LOGI(TAG, "USB Drive OFF: firmware owns FATFS");
    return true;
}

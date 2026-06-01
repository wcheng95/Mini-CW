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

static const char *TAG = "storage_service";

#define STORAGE_FATFS_LABEL "fatfs"
#define STORAGE_FATFS_BASE_PATH "/fatfs"
#define STORAGE_FATFS_MAX_FILES 4
#define STORAGE_FATFS_ALLOC_UNIT 4096

static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;
static bool s_storage_ready;
static bool s_fatfs_mounted;
static bool s_usb_drive_enabled;
static bool s_tinyusb_installed;

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
    if (!storage_firmware_can_access_fatfs("profile load")) {
        return false;
    }

    ESP_LOGI(TAG, "profile load skipped: persistence disabled");
    return false;
}

bool storage_profile_save(void)
{
    if (!storage_firmware_can_access_fatfs("profile save")) {
        return false;
    }

    ESP_LOGI(TAG, "profile save skipped: persistence disabled");
    return false;
}

bool storage_session_log_append(const char *line)
{
    if (!storage_firmware_can_access_fatfs("session log")) {
        return false;
    }

    ESP_LOGI(TAG, "session log skipped: %s", line ? line : "");
    return false;
}

bool storage_lesson_load(cw_lesson_config_t *config, cw_lesson_result_t *result)
{
    (void)config;
    (void)result;

    if (!storage_firmware_can_access_fatfs("lesson load")) {
        return false;
    }

    ESP_LOGI(TAG, "lesson load skipped: persistence disabled");
    return false;
}

bool storage_lesson_save_config(const cw_lesson_config_t *config)
{
    (void)config;

    if (!storage_firmware_can_access_fatfs("lesson config save")) {
        return false;
    }

    ESP_LOGI(TAG, "lesson config save skipped: persistence disabled");
    return false;
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
    (void)config;
    (void)result;

    if (!storage_firmware_can_access_fatfs("word load")) {
        return false;
    }

    ESP_LOGI(TAG, "word load skipped: persistence disabled");
    return false;
}

bool storage_word_save_config(const cw_word_config_t *config)
{
    (void)config;

    if (!storage_firmware_can_access_fatfs("word config save")) {
        return false;
    }

    ESP_LOGI(TAG, "word config save skipped: persistence disabled");
    return false;
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
    (void)config;
    (void)result;

    if (!storage_firmware_can_access_fatfs("callsign load")) {
        return false;
    }

    ESP_LOGI(TAG, "callsign load skipped: persistence disabled");
    return false;
}

bool storage_callsign_save_config(const cw_callsign_config_t *config)
{
    (void)config;

    if (!storage_firmware_can_access_fatfs("callsign config save")) {
        return false;
    }

    ESP_LOGI(TAG, "callsign config save skipped: persistence disabled");
    return false;
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
    (void)config;
    (void)result;

    if (!storage_firmware_can_access_fatfs("plaintext load")) {
        return false;
    }

    ESP_LOGI(TAG, "plaintext load skipped: persistence disabled");
    return false;
}

bool storage_plaintext_save_config(const cw_plaintext_config_t *config)
{
    (void)config;

    if (!storage_firmware_can_access_fatfs("plaintext config save")) {
        return false;
    }

    ESP_LOGI(TAG, "plaintext config save skipped: persistence disabled");
    return false;
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

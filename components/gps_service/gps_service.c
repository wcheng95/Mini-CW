/*
 * gps_service
 *
 * PortA-only NMEA GPS parser with 9600/115200 baud auto-detection.
 */

#include "gps_service.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "gps_service";

#define GPS_SERVICE_UART UART_NUM_1
#define GPS_SERVICE_RX_PIN GPIO_NUM_1
#define GPS_SERVICE_TX_PIN GPIO_NUM_2
#define GPS_SERVICE_BAUD_FAST 115200
#define GPS_SERVICE_BAUD_SLOW 9600
#define GPS_SERVICE_LINE_MAX 128U
#define GPS_SERVICE_PROBE_WINDOW_MS 2500U
#define GPS_SERVICE_UART_RX_BUF_SIZE 2048
#define GPS_SERVICE_GRID8_LEN 8U

static gps_service_state_t s_state;
static char s_line_buffer[GPS_SERVICE_LINE_MAX + 1U];
static size_t s_line_len;
static uint32_t s_probe_start_ms;
static uint32_t s_probe_rx_bytes;
static bool s_probe_decodable;
static int s_reported_good_baud = GPS_SERVICE_BAUD_FAST;
static bool s_pending_baud_update;
static int s_pending_baud_value;

static uint32_t gps_service_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static int gps_service_normalize_baud(int baud)
{
    return baud == GPS_SERVICE_BAUD_SLOW ? GPS_SERVICE_BAUD_SLOW : GPS_SERVICE_BAUD_FAST;
}

static int gps_service_other_baud(int baud)
{
    return gps_service_normalize_baud(baud) == GPS_SERVICE_BAUD_FAST ? GPS_SERVICE_BAUD_SLOW
                                                                    : GPS_SERVICE_BAUD_FAST;
}

static void gps_service_rearm_probe_window(void)
{
    s_probe_start_ms = gps_service_now_ms();
    s_probe_rx_bytes = 0U;
    s_probe_decodable = false;
    s_line_len = 0U;
    s_line_buffer[0] = '\0';
}

static bool gps_service_is_leap_year(uint16_t year)
{
    return (year % 4U == 0U) && ((year % 100U) != 0U || (year % 400U) == 0U);
}

static uint8_t gps_service_days_in_month(uint16_t year, uint8_t month)
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

    if (month == 2U && gps_service_is_leap_year(year)) {
        return 29U;
    }

    return days[month - 1U];
}

static bool gps_service_parse_two_digits(const char *text, uint8_t *out_value)
{
    if (text == NULL || out_value == NULL || !isdigit((unsigned char)text[0]) ||
        !isdigit((unsigned char)text[1])) {
        return false;
    }

    *out_value = (uint8_t)((text[0] - '0') * 10 + (text[1] - '0'));
    return true;
}

static bool gps_service_format_rmc_time(const char *time_field, char *dest, size_t dest_size)
{
    uint8_t hour;
    uint8_t minute;
    uint8_t second;

    if (time_field == NULL || dest == NULL || dest_size < sizeof("HH:MM:SS") ||
        strlen(time_field) < 6U) {
        return false;
    }

    if (!gps_service_parse_two_digits(&time_field[0], &hour) ||
        !gps_service_parse_two_digits(&time_field[2], &minute) ||
        !gps_service_parse_two_digits(&time_field[4], &second)) {
        return false;
    }

    if (hour > 23U || minute > 59U || second > 59U) {
        return false;
    }

    snprintf(dest, dest_size, "%02u:%02u:%02u", (unsigned)hour, (unsigned)minute, (unsigned)second);
    return true;
}

static bool gps_service_format_rmc_date(const char *date_field, char *dest, size_t dest_size)
{
    uint8_t day;
    uint8_t month;
    uint8_t yy;
    uint16_t year;

    if (date_field == NULL || dest == NULL || dest_size < sizeof("YYYY-MM-DD") ||
        strlen(date_field) < 6U) {
        return false;
    }

    if (!gps_service_parse_two_digits(&date_field[0], &day) ||
        !gps_service_parse_two_digits(&date_field[2], &month) ||
        !gps_service_parse_two_digits(&date_field[4], &yy)) {
        return false;
    }

    year = (uint16_t)(2000U + yy);
    if (year < 2024U || year > 2099U || month < 1U || month > 12U || day < 1U ||
        day > gps_service_days_in_month(year, month)) {
        return false;
    }

    snprintf(dest,
             dest_size,
             "%04u-%02u-%02u",
             (unsigned)year,
             (unsigned)month,
             (unsigned)day);
    return true;
}

static bool gps_service_parse_nmea_coord(const char *value,
                                         const char *direction,
                                         bool longitude,
                                         double *out_degrees)
{
    char *end = NULL;
    double raw;
    int degrees;
    double minutes;
    double result;

    if (value == NULL || direction == NULL || out_degrees == NULL ||
        value[0] == '\0' || direction[0] == '\0') {
        return false;
    }

    if (longitude) {
        if (direction[0] != 'E' && direction[0] != 'W') {
            return false;
        }
    } else if (direction[0] != 'N' && direction[0] != 'S') {
        return false;
    }

    raw = strtod(value, &end);
    if (end == value) {
        return false;
    }

    degrees = (int)(raw / 100.0);
    minutes = raw - (double)(degrees * 100);
    if (minutes < 0.0 || minutes >= 60.0) {
        return false;
    }

    result = (double)degrees + minutes / 60.0;
    if (direction[0] == 'S' || direction[0] == 'W') {
        result = -result;
    }

    if (longitude) {
        if (result < -180.0 || result > 180.0) {
            return false;
        }
    } else if (result < -90.0 || result > 90.0) {
        return false;
    }

    *out_degrees = result;
    return true;
}

static int gps_service_clamp_int(int value, int min_value, int max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static bool gps_service_format_grid8(double latitude, double longitude, char *dest, size_t dest_size)
{
    int field_lon;
    int field_lat;
    int square_lon;
    int square_lat;
    int sub_lon;
    int sub_lat;
    int ext_lon;
    int ext_lat;
    const double sub_lon_w = 2.0 / 24.0;
    const double sub_lat_h = 1.0 / 24.0;
    const double ext_lon_w = sub_lon_w / 10.0;
    const double ext_lat_h = sub_lat_h / 10.0;

    if (dest == NULL || dest_size < GPS_SERVICE_GRID8_LEN + 1U ||
        longitude < -180.0 || longitude > 180.0 ||
        latitude < -90.0 || latitude > 90.0) {
        return false;
    }

    if (longitude >= 180.0) {
        longitude = 179.999999;
    }
    if (latitude >= 90.0) {
        latitude = 89.999999;
    }

    longitude += 180.0;
    latitude += 90.0;

    field_lon = (int)(longitude / 20.0);
    field_lat = (int)(latitude / 10.0);
    longitude -= (double)field_lon * 20.0;
    latitude -= (double)field_lat * 10.0;

    square_lon = (int)(longitude / 2.0);
    square_lat = (int)(latitude / 1.0);
    longitude -= (double)square_lon * 2.0;
    latitude -= (double)square_lat * 1.0;

    sub_lon = (int)(longitude / sub_lon_w);
    sub_lat = (int)(latitude / sub_lat_h);
    longitude -= (double)sub_lon * sub_lon_w;
    latitude -= (double)sub_lat * sub_lat_h;

    ext_lon = (int)(longitude / ext_lon_w);
    ext_lat = (int)(latitude / ext_lat_h);

    field_lon = gps_service_clamp_int(field_lon, 0, 17);
    field_lat = gps_service_clamp_int(field_lat, 0, 17);
    square_lon = gps_service_clamp_int(square_lon, 0, 9);
    square_lat = gps_service_clamp_int(square_lat, 0, 9);
    sub_lon = gps_service_clamp_int(sub_lon, 0, 23);
    sub_lat = gps_service_clamp_int(sub_lat, 0, 23);
    ext_lon = gps_service_clamp_int(ext_lon, 0, 9);
    ext_lat = gps_service_clamp_int(ext_lat, 0, 9);

    dest[0] = (char)('A' + field_lon);
    dest[1] = (char)('A' + field_lat);
    dest[2] = (char)('0' + square_lon);
    dest[3] = (char)('0' + square_lat);
    dest[4] = (char)('a' + sub_lon);
    dest[5] = (char)('a' + sub_lat);
    dest[6] = (char)('0' + ext_lon);
    dest[7] = (char)('0' + ext_lat);
    dest[8] = '\0';
    return true;
}

static int gps_service_from_hex(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    return -1;
}

static bool gps_service_nmea_checksum_ok(const char *line, char *payload, size_t payload_size)
{
    const char *star;
    size_t payload_len;
    uint8_t parity = 0U;
    int high;
    int low;

    if (line == NULL || payload == NULL || payload_size == 0U || line[0] != '$') {
        return false;
    }

    star = strchr(line, '*');
    if (star == NULL || star == line + 1 || star[1] == '\0' || star[2] == '\0') {
        return false;
    }

    high = gps_service_from_hex(star[1]);
    low = gps_service_from_hex(star[2]);
    if (high < 0 || low < 0) {
        return false;
    }

    for (const char *cursor = line + 1; cursor < star; ++cursor) {
        parity ^= (uint8_t)*cursor;
    }

    if (parity != (uint8_t)((high << 4) | low)) {
        return false;
    }

    payload_len = (size_t)(star - (line + 1));
    if (payload_len + 1U > payload_size) {
        return false;
    }

    memcpy(payload, line + 1, payload_len);
    payload[payload_len] = '\0';
    return true;
}

static size_t gps_service_split_csv(char *text, char **fields, size_t max_fields)
{
    size_t count = 0U;

    if (text == NULL || fields == NULL || max_fields == 0U) {
        return 0U;
    }

    fields[count++] = text;
    for (char *cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor == ',') {
            *cursor = '\0';
            if (count < max_fields) {
                fields[count++] = cursor + 1;
            }
        }
    }

    return count;
}

static bool gps_service_sentence_type_ends_with(const char *type, const char *suffix)
{
    size_t type_len;
    size_t suffix_len;

    if (type == NULL || suffix == NULL) {
        return false;
    }

    type_len = strlen(type);
    suffix_len = strlen(suffix);
    return type_len >= suffix_len && strcmp(type + type_len - suffix_len, suffix) == 0;
}

static bool gps_service_configure_uart(int baud)
{
    uart_config_t cfg = {
        .baud_rate = gps_service_normalize_baud(baud),
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
#ifdef UART_SCLK_REF_TICK
        .source_clk = UART_SCLK_REF_TICK,
#else
        .source_clk = UART_SCLK_DEFAULT,
#endif
    };
    esp_err_t err;

    err = uart_param_config(GPS_SERVICE_UART, &cfg);
    if (err != ESP_OK) {
        return false;
    }

    err = uart_set_pin(GPS_SERVICE_UART,
                       GPS_SERVICE_TX_PIN,
                       GPS_SERVICE_RX_PIN,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return false;
    }

    err = uart_set_line_inverse(GPS_SERVICE_UART, (uart_signal_inv_t)0);
    if (err != ESP_OK) {
        return false;
    }

    err = uart_set_baudrate(GPS_SERVICE_UART, gps_service_normalize_baud(baud));
    if (err != ESP_OK) {
        return false;
    }

    uart_flush_input(GPS_SERVICE_UART);
    return true;
}

static bool gps_service_ensure_uart_driver(void)
{
    esp_err_t err = uart_driver_install(GPS_SERVICE_UART,
                                        GPS_SERVICE_UART_RX_BUF_SIZE,
                                        0,
                                        0,
                                        NULL,
                                        0);

    if (err == ESP_ERR_INVALID_STATE) {
        uart_driver_delete(GPS_SERVICE_UART);
        err = uart_driver_install(GPS_SERVICE_UART,
                                  GPS_SERVICE_UART_RX_BUF_SIZE,
                                  0,
                                  0,
                                  NULL,
                                  0);
    }

    return err == ESP_OK;
}

static void gps_service_switch_baud(int baud)
{
    baud = gps_service_normalize_baud(baud);
    if (uart_set_baudrate(GPS_SERVICE_UART, baud) != ESP_OK) {
        return;
    }

    uart_flush_input(GPS_SERVICE_UART);
    s_state.active_baud = baud;
    s_state.baud_locked = false;
    gps_service_rearm_probe_window();
    ESP_LOGI(TAG, "GPS probe switch baud=%d", baud);
}

static void gps_service_lock_current_baud_if_needed(void)
{
    if (s_state.baud_locked) {
        return;
    }

    s_state.baud_locked = true;
    if (s_state.active_baud != s_reported_good_baud) {
        s_reported_good_baud = s_state.active_baud;
        s_pending_baud_value = s_state.active_baud;
        s_pending_baud_update = true;
    }

    ESP_LOGI(TAG, "GPS baud lock=%d", s_state.active_baud);
}

static bool gps_service_parse_sentence(const char *raw_line)
{
    char payload[GPS_SERVICE_LINE_MAX + 1U];
    char *fields[20];
    size_t field_count;
    const char *type;
    double latitude;
    double longitude;

    if (!gps_service_nmea_checksum_ok(raw_line, payload, sizeof(payload))) {
        return false;
    }

    field_count = gps_service_split_csv(payload, fields, sizeof(fields) / sizeof(fields[0]));
    if (field_count == 0U) {
        return false;
    }

    type = fields[0];
    if (gps_service_sentence_type_ends_with(type, "RMC") && field_count >= 10U) {
        if (strcmp(fields[2], "A") == 0 &&
            gps_service_format_rmc_time(fields[1], s_state.time_utc, sizeof(s_state.time_utc)) &&
            gps_service_format_rmc_date(fields[9], s_state.date_utc, sizeof(s_state.date_utc)) &&
            gps_service_parse_nmea_coord(fields[3], fields[4], false, &latitude) &&
            gps_service_parse_nmea_coord(fields[5], fields[6], true, &longitude) &&
            gps_service_format_grid8(latitude, longitude, s_state.grid8, sizeof(s_state.grid8))) {
            s_state.valid_fix = true;
        } else {
            s_state.valid_fix = false;
            s_state.date_utc[0] = '\0';
            s_state.time_utc[0] = '\0';
            s_state.grid8[0] = '\0';
        }
    } else if (gps_service_sentence_type_ends_with(type, "GGA") && field_count >= 8U) {
        if (fields[7][0] != '\0') {
            int satellites = atoi(fields[7]);
            if (satellites < 0) {
                satellites = 0;
            } else if (satellites > UINT8_MAX) {
                satellites = UINT8_MAX;
            }
            s_state.satellites = (uint8_t)satellites;
        }
    }

    s_state.last_rx_ms = gps_service_now_ms();
    s_probe_decodable = true;
    gps_service_lock_current_baud_if_needed();
    return true;
}

static void gps_service_ingest_uart_bytes(const uint8_t *data, int len)
{
    if (data == NULL || len <= 0) {
        return;
    }

    s_probe_rx_bytes += (uint32_t)len;
    for (int i = 0; i < len; ++i) {
        char ch = (char)data[i];

        if (ch == '\r' || ch == '\n') {
            if (s_line_len > 0U) {
                s_line_buffer[s_line_len] = '\0';
                (void)gps_service_parse_sentence(s_line_buffer);
                s_line_len = 0U;
                s_line_buffer[0] = '\0';
            }
            continue;
        }

        if ((unsigned char)ch < 32U || (unsigned char)ch > 126U) {
            continue;
        }

        if (s_line_len >= GPS_SERVICE_LINE_MAX) {
            s_line_len = 0U;
            s_line_buffer[0] = '\0';
            continue;
        }

        s_line_buffer[s_line_len++] = ch;
    }
}

void gps_service_start(int preload_baud)
{
    int baud = gps_service_normalize_baud(preload_baud);

    if (s_state.running) {
        return;
    }

    if (!gps_service_ensure_uart_driver()) {
        ESP_LOGW(TAG, "GPS UART driver install failed");
        return;
    }

    if (!gps_service_configure_uart(baud)) {
        ESP_LOGW(TAG, "GPS UART config failed");
        uart_driver_delete(GPS_SERVICE_UART);
        return;
    }

    memset(&s_state, 0, sizeof(s_state));
    s_state.running = true;
    s_state.active_baud = baud;
    s_state.baud_locked = false;
    s_reported_good_baud = baud;
    s_pending_baud_update = false;
    s_pending_baud_value = 0;
    gps_service_rearm_probe_window();

    ESP_LOGI(TAG,
             "GPS started on UART%d TX=G%d RX=G%d preload=%d auto=1",
             (int)GPS_SERVICE_UART,
             (int)GPS_SERVICE_TX_PIN,
             (int)GPS_SERVICE_RX_PIN,
             baud);
}

void gps_service_stop(void)
{
    if (!s_state.running) {
        return;
    }

    uart_flush_input(GPS_SERVICE_UART);
    uart_driver_delete(GPS_SERVICE_UART);
    memset(&s_state, 0, sizeof(s_state));
    s_line_len = 0U;
    s_line_buffer[0] = '\0';
    s_probe_start_ms = 0U;
    s_probe_rx_bytes = 0U;
    s_probe_decodable = false;
    s_pending_baud_update = false;
    s_pending_baud_value = 0;
    ESP_LOGI(TAG, "GPS stopped");
}

void gps_service_update(void)
{
    uint8_t buf[256];
    int len;

    if (!s_state.running) {
        return;
    }

    len = uart_read_bytes(GPS_SERVICE_UART, buf, sizeof(buf), 0);
    if (len > 0) {
        gps_service_ingest_uart_bytes(buf, len);
    }

    if (!s_state.baud_locked) {
        uint32_t now = gps_service_now_ms();
        if (s_probe_rx_bytes > 0U && now - s_probe_start_ms >= GPS_SERVICE_PROBE_WINDOW_MS &&
            !s_probe_decodable) {
            gps_service_switch_baud(gps_service_other_baud(s_state.active_baud));
        }
    }
}

bool gps_service_get_state(gps_service_state_t *out_state)
{
    if (out_state == NULL) {
        return false;
    }

    *out_state = s_state;
    return true;
}

bool gps_service_take_baud_update(int *out_baud)
{
    if (!s_pending_baud_update) {
        return false;
    }

    if (out_baud != NULL) {
        *out_baud = s_pending_baud_value;
    }
    s_pending_baud_update = false;
    s_pending_baud_value = 0;
    return true;
}

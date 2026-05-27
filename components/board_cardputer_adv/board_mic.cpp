#include "board_mic.h"
#include "board_pins.h"

#include <string.h>

#include "esp_log.h"
#include "esp_check.h"

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"

#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

static const char* TAG = "BOARD_MIC";

static i2c_master_bus_handle_t g_i2c_bus = nullptr;
static i2s_chan_handle_t g_i2s_rx = nullptr;
static esp_codec_dev_handle_t g_codec = nullptr;

static board_mic_config_t g_cfg = {
    .sample_rate = 48000,
    .channels = 1,
    .bits_per_sample = 16,
    .gain_db = 20.0f,
};

static bool g_initialized = false;

static esp_err_t board_mic_init_i2c(void)
{
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = BOARD_I2C_SDA;
    bus_cfg.scl_io_num = BOARD_I2C_SCL;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &g_i2c_bus);

    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "I2C bus already exists; reusing I2C_NUM_0");

        err = i2c_master_get_bus_handle(I2C_NUM_0, &g_i2c_bus);
        ESP_RETURN_ON_ERROR(err, TAG, "i2c_master_get_bus_handle failed");
    } else {
        ESP_RETURN_ON_ERROR(err, TAG, "i2c_new_master_bus failed");
    }

    ESP_LOGI(TAG, "I2C init OK: SDA=%d SCL=%d", BOARD_I2C_SDA, BOARD_I2C_SCL);
    return ESP_OK;
}

static esp_err_t board_mic_init_i2s_rx(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);

    ESP_RETURN_ON_ERROR(
        i2s_new_channel(&chan_cfg, nullptr, &g_i2s_rx),
        TAG,
        "i2s_new_channel RX failed"
    );

    i2s_std_config_t std_cfg = {};

    const uint32_t sample_rate_hz = (uint32_t)g_cfg.sample_rate;

    std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz);
    std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT,
        I2S_SLOT_MODE_MONO
    );

    std_cfg.gpio_cfg.mclk = BOARD_I2S_MCLK;
    std_cfg.gpio_cfg.bclk = BOARD_I2S_BCLK;
    std_cfg.gpio_cfg.ws = BOARD_I2S_WS;
    std_cfg.gpio_cfg.dout = GPIO_NUM_NC;
    std_cfg.gpio_cfg.din = BOARD_I2S_DIN;
    std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.ws_inv = false;

    ESP_RETURN_ON_ERROR(
        i2s_channel_init_std_mode(g_i2s_rx, &std_cfg),
        TAG,
        "i2s_channel_init_std_mode failed"
    );

    ESP_LOGI(TAG,
             "I2S RX init OK: BCLK=%d WS=%d DIN=%d sample_rate=%d",
             BOARD_I2S_BCLK,
             BOARD_I2S_WS,
             BOARD_I2S_DIN,
             g_cfg.sample_rate);

    return ESP_OK;
}

static esp_err_t board_mic_init_codec(void)
{
    audio_codec_i2s_cfg_t i2s_cfg = {};
    i2s_cfg.rx_handle = g_i2s_rx;

    const audio_codec_data_if_t* data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(data_if, ESP_FAIL, TAG, "audio_codec_new_i2s_data failed");

    audio_codec_i2c_cfg_t i2c_cfg = {};
    i2c_cfg.addr = ES8311_CODEC_DEFAULT_ADDR;
    i2c_cfg.bus_handle = g_i2c_bus;

    const audio_codec_ctrl_if_t* ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(ctrl_if, ESP_FAIL, TAG, "audio_codec_new_i2c_ctrl failed");

    const audio_codec_gpio_if_t* gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(gpio_if, ESP_FAIL, TAG, "audio_codec_new_gpio failed");

    es8311_codec_cfg_t es8311_cfg = {};
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
    es8311_cfg.ctrl_if = ctrl_if;
    es8311_cfg.gpio_if = gpio_if;
    es8311_cfg.pa_pin = GPIO_NUM_NC;
    es8311_cfg.use_mclk = false;

    const audio_codec_if_t* codec_if = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(codec_if, ESP_FAIL, TAG, "es8311_codec_new failed");

    esp_codec_dev_cfg_t dev_cfg = {};
    dev_cfg.codec_if = codec_if;
    dev_cfg.data_if = data_if;
    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;

    g_codec = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(g_codec, ESP_FAIL, TAG, "esp_codec_dev_new failed");

    esp_codec_dev_sample_info_t fs = {};
    fs.sample_rate = g_cfg.sample_rate;
    fs.channel = g_cfg.channels;
    fs.bits_per_sample = g_cfg.bits_per_sample;

    ESP_RETURN_ON_ERROR(
        esp_codec_dev_open(g_codec, &fs),
        TAG,
        "esp_codec_dev_open failed"
    );

    ESP_RETURN_ON_ERROR(
        esp_codec_dev_set_in_gain(g_codec, g_cfg.gain_db),
        TAG,
        "esp_codec_dev_set_in_gain failed"
    );

    ESP_LOGI(TAG,
             "ES8311 codec opened: rate=%d channels=%d bits=%d gain=%.1f dB use_mclk=0",
             g_cfg.sample_rate,
             g_cfg.channels,
             g_cfg.bits_per_sample,
             (double)g_cfg.gain_db);

    return ESP_OK;
}

esp_err_t board_mic_init(const board_mic_config_t* cfg)
{
    if (g_initialized) {
        ESP_LOGW(TAG, "board_mic_init called but mic is already initialized");
        return ESP_OK;
    }

    if (cfg) {
        g_cfg = *cfg;
    }

    if (g_cfg.sample_rate <= 0) {
        g_cfg.sample_rate = 48000;
    }

    if (g_cfg.channels <= 0) {
        g_cfg.channels = 1;
    }

    if (g_cfg.bits_per_sample <= 0) {
        g_cfg.bits_per_sample = 16;
    }

    esp_err_t err = ESP_OK;

    err = board_mic_init_i2c();
    if (err != ESP_OK) {
        board_mic_deinit();
        return err;
    }

    err = board_mic_init_i2s_rx();
    if (err != ESP_OK) {
        board_mic_deinit();
        return err;
    }

    err = board_mic_init_codec();
    if (err != ESP_OK) {
        board_mic_deinit();
        return err;
    }

    // Discard startup garbage frames.
    // Our test showed frame 0 often contains full-scale garbage.
    int16_t throwaway[480];

    for (int i = 0; i < 3; ++i) {
        memset(throwaway, 0, sizeof(throwaway));
        (void)esp_codec_dev_read(g_codec, throwaway, sizeof(throwaway));
    }

    ESP_LOGI(TAG, "discarded first 3 startup frames");

    g_initialized = true;
    return ESP_OK;
}

esp_err_t board_mic_read(int16_t* samples, size_t sample_count, size_t* bytes_read)
{
    if (!g_initialized || !g_codec) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!samples || sample_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const int bytes_to_read = (int)(sample_count * sizeof(int16_t));

    int ret = esp_codec_dev_read(g_codec, (void*)samples, bytes_to_read);

    if (ret == ESP_CODEC_DEV_OK) {
        if (bytes_read) {
            *bytes_read = bytes_to_read;
        }
        return ESP_OK;
    }

    if (bytes_read) {
        *bytes_read = 0;
    }

    return ESP_FAIL;
}

void board_mic_deinit(void)
{
    if (g_codec) {
        esp_codec_dev_close(g_codec);
        g_codec = nullptr;
    }

    if (g_i2s_rx) {
        (void)i2s_channel_disable(g_i2s_rx);
        (void)i2s_del_channel(g_i2s_rx);
        g_i2s_rx = nullptr;
    }

    if (g_i2c_bus) {
        //i2c_del_master_bus(g_i2c_bus);
        g_i2c_bus = nullptr;
    }

    g_initialized = false;
}

bool board_mic_is_initialized(void)
{
    return g_initialized;
}
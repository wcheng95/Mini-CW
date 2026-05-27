#include "board_speaker.h"
#include "board_pins.h"

#include <string.h>

#include "esp_log.h"
#include "esp_check.h"

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"

#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

static const char* TAG = "BOARD_SPK";

static i2c_master_bus_handle_t g_i2c_bus = nullptr;
static i2s_chan_handle_t g_i2s_tx = nullptr;
static esp_codec_dev_handle_t g_codec = nullptr;

static board_speaker_config_t g_cfg = {
    .sample_rate = 48000,
    .channels = 1,
    .bits_per_sample = 16,
    .volume = 60,
};

static bool g_initialized = false;

static esp_err_t board_speaker_init_i2c(void)
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

static esp_err_t board_speaker_init_i2s_tx(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);

    ESP_RETURN_ON_ERROR(
        i2s_new_channel(&chan_cfg, &g_i2s_tx, nullptr),
        TAG,
        "i2s_new_channel TX failed"
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
    std_cfg.gpio_cfg.dout = BOARD_I2S_DOUT;
    std_cfg.gpio_cfg.din = GPIO_NUM_NC;
    std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.ws_inv = false;

    ESP_RETURN_ON_ERROR(
        i2s_channel_init_std_mode(g_i2s_tx, &std_cfg),
        TAG,
        "i2s_channel_init_std_mode TX failed"
    );

    ESP_LOGI(TAG,
             "I2S TX init OK: BCLK=%d WS=%d DOUT=%d sample_rate=%d",
             BOARD_I2S_BCLK,
             BOARD_I2S_WS,
             BOARD_I2S_DOUT,
             g_cfg.sample_rate);

    return ESP_OK;
}

static esp_err_t board_speaker_init_codec(void)
{
    audio_codec_i2s_cfg_t i2s_cfg = {};
    i2s_cfg.tx_handle = g_i2s_tx;

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
    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_OUT;

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
        esp_codec_dev_set_out_vol(g_codec, g_cfg.volume),
        TAG,
        "esp_codec_dev_set_out_vol failed"
    );

    ESP_LOGI(TAG,
             "ES8311 speaker opened: rate=%d channels=%d bits=%d volume=%d use_mclk=0",
             g_cfg.sample_rate,
             g_cfg.channels,
             g_cfg.bits_per_sample,
             g_cfg.volume);

    return ESP_OK;
}

esp_err_t board_speaker_init(const board_speaker_config_t* cfg)
{
    if (g_initialized) {
        ESP_LOGW(TAG, "board_speaker_init called but already initialized");
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

    if (g_cfg.volume < 0) {
        g_cfg.volume = 0;
    }

    if (g_cfg.volume > 100) {
        g_cfg.volume = 100;
    }

    esp_err_t err = ESP_OK;

    err = board_speaker_init_i2c();
    if (err != ESP_OK) {
        board_speaker_deinit();
        return err;
    }

    err = board_speaker_init_i2s_tx();
    if (err != ESP_OK) {
        board_speaker_deinit();
        return err;
    }

    err = board_speaker_init_codec();
    if (err != ESP_OK) {
        board_speaker_deinit();
        return err;
    }

    g_initialized = true;
    return ESP_OK;
}

esp_err_t board_speaker_write(const int16_t* samples,
                              size_t sample_count,
                              size_t* bytes_written)
{
    if (!g_initialized || !g_codec) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!samples || sample_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const int bytes_to_write = (int)(sample_count * sizeof(int16_t));

    int ret = esp_codec_dev_write(g_codec, (void*)samples, bytes_to_write);

    if (ret == ESP_CODEC_DEV_OK) {
        if (bytes_written) {
            *bytes_written = bytes_to_write;
        }
        return ESP_OK;
    }

    if (bytes_written) {
        *bytes_written = 0;
    }

    return ESP_FAIL;
}

void board_speaker_deinit(void)
{
    if (g_codec) {
        esp_codec_dev_close(g_codec);
        g_codec = nullptr;
    }

    if (g_i2s_tx) {
        (void)i2s_channel_disable(g_i2s_tx);
        (void)i2s_del_channel(g_i2s_tx);
        g_i2s_tx = nullptr;
    }

    if (g_i2c_bus) {
        //i2c_del_master_bus(g_i2c_bus);
        g_i2c_bus = nullptr;
    }

    g_initialized = false;
}

bool board_speaker_is_initialized(void)
{
    return g_initialized;
}
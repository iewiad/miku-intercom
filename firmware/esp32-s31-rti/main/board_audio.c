#include "board_audio.h"

#include "bsp/esp-bsp.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "nvs.h"

#define RTI_AUDIO_SAMPLE_RATE_HZ 16000
#define RTI_AUDIO_DEFAULT_VOLUME 55
#define RTI_SETTINGS_NAMESPACE "rti_settings"
#define RTI_VOLUME_KEY "volume"

static const char *TAG = "board_audio";

static uint8_t load_output_volume(void)
{
    uint8_t volume = RTI_AUDIO_DEFAULT_VOLUME;
    nvs_handle_t handle;
    if (nvs_open(RTI_SETTINGS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        uint8_t saved = RTI_AUDIO_DEFAULT_VOLUME;
        if (nvs_get_u8(handle, RTI_VOLUME_KEY, &saved) == ESP_OK && saved <= 100) {
            volume = saved;
        }
        nvs_close(handle);
    }
    return volume;
}

esp_err_t board_audio_set_output_volume(board_audio_t *audio, uint8_t volume, bool persist)
{
    if (audio == NULL || audio->speaker == NULL || volume > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    if (esp_codec_dev_set_out_vol(audio->speaker, volume) != 0) {
        return ESP_FAIL;
    }
    audio->output_volume = volume;

    if (persist) {
        nvs_handle_t handle;
        esp_err_t err = nvs_open(RTI_SETTINGS_NAMESPACE, NVS_READWRITE, &handle);
        if (err != ESP_OK) {
            return err;
        }
        err = nvs_set_u8(handle, RTI_VOLUME_KEY, volume);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
        if (err != ESP_OK) {
            return err;
        }
    }
    ESP_LOGI(TAG, "speaker volume set to %u%%%s", (unsigned)volume,
             persist ? " and saved" : "");
    return ESP_OK;
}

uint8_t board_audio_get_output_volume(const board_audio_t *audio)
{
    return audio == NULL ? RTI_AUDIO_DEFAULT_VOLUME : audio->output_volume;
}

esp_err_t board_audio_init(board_audio_t *audio)
{
    if (audio == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const i2s_std_config_t i2s_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RTI_AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    esp_err_t err = bsp_audio_init(&i2s_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S initialization failed: %s", esp_err_to_name(err));
        return err;
    }

    audio->speaker = bsp_audio_codec_speaker_init();
    audio->microphone = bsp_audio_codec_microphone_init();
    if (audio->speaker == NULL || audio->microphone == NULL) {
        ESP_LOGE(TAG, "ES8389 codec initialization failed");
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t sample_info = {
        .sample_rate = RTI_AUDIO_SAMPLE_RATE_HZ,
        .channel = 2,
        .bits_per_sample = 16,
    };
    if (esp_codec_dev_open(audio->speaker, &sample_info) != 0 ||
        esp_codec_dev_open(audio->microphone, &sample_info) != 0) {
        ESP_LOGE(TAG, "could not open ES8389 capture/playback streams");
        return ESP_FAIL;
    }
    audio->output_volume = load_output_volume();
    if (esp_codec_dev_set_out_vol(audio->speaker, audio->output_volume) != 0) {
        ESP_LOGE(TAG, "could not set ES8389 output volume");
        return ESP_FAIL;
    }
    esp_codec_dev_set_in_gain(audio->microphone, 24.0f);

    ESP_LOGI(TAG, "ES8389 ready: 16 kHz, 16-bit, stereo I2S, volume=%u%%",
             (unsigned)audio->output_volume);
    return ESP_OK;
}

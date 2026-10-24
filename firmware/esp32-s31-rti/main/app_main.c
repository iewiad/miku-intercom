#include "app_state.h"
#include <string.h>

#include "board_audio.h"
#include "board_display.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs_flash.h"
#include "rti_controller.h"
#include "group_config.h"
#include "group_controller.h"
#include "sdkconfig.h"
#include "webrtc_peer.h"
#include "wifi_manager.h"
#include "wifi_serial_cli.h"

static const char *TAG = "miku_rti_s31";
static board_audio_t board_audio;

static esp_err_t enter_voice_after_wifi(void *context)
{
#if CONFIG_MIKU_INTERCOM_DEFAULT_MODE
    return group_controller_init((const board_audio_t *)context);
#else
    return rti_controller_init((const board_audio_t *)context);
#endif
}

void app_main(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "booting Shenglu voice device on %d-core ESP32-S31", chip_info.cores);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
#if CONFIG_MIKU_INTERCOM_DEFAULT_MODE
        /* A group application update must not erase existing Wi-Fi/RTA data. */
        ESP_LOGE(TAG, "NVS requires explicit repair; preserving all stored configuration");
        return;
#else
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
#endif
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(board_display_init());

    ESP_ERROR_CHECK(board_audio_init(&board_audio));

#if CONFIG_MIKU_RTI_AEC_BOOT_SELF_TEST
    err = webrtc_peer_aec_self_test();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AEC boot self-test failed: %s; runtime will use its fallback",
                 esp_err_to_name(err));
    }
#endif

    err = webrtc_peer_prepare();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DTLS certificate pre-generation failed: %s; RTC may reconnect slowly",
                 esp_err_to_name(err));
    }

    ESP_ERROR_CHECK(wifi_manager_init());
    ESP_ERROR_CHECK(wifi_serial_cli_start());
    err = wifi_manager_has_saved_credentials()
              ? wifi_manager_connect_saved()
              : ESP_ERR_NOT_FOUND;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "device needs Wi-Fi setup: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(board_display_show_wifi_setup(enter_voice_after_wifi, &board_audio));
        return;
    }
    ESP_ERROR_CHECK(enter_voice_after_wifi(&board_audio));
}

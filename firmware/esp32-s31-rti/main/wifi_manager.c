#include "wifi_manager.h"

#include <stdbool.h>
#include <string.h>

#include "app_state.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT BIT1
#define WIFI_MAX_RETRIES 5
#define WIFI_CONNECT_TIMEOUT_MS 20000

static const char *TAG = "wifi_manager";
static EventGroupHandle_t wifi_events;
static SemaphoreHandle_t wifi_operation_mutex;
static bool initialized;
static bool connection_requested;
static int retry_count;

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!connection_requested) {
            return;
        }

        if (retry_count++ < WIFI_MAX_RETRIES) {
            ESP_LOGW(TAG, "connection lost; retry %d/%d", retry_count, WIFI_MAX_RETRIES);
            esp_wifi_connect();
        } else {
            connection_requested = false;
            xEventGroupSetBits(wifi_events, WIFI_FAILED_BIT);
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        retry_count = 0;
        ESP_LOGI(TAG, "connected, ip=" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_manager_init(void)
{
    if (initialized) {
        return ESP_OK;
    }

    wifi_events = xEventGroupCreate();
    wifi_operation_mutex = xSemaphoreCreateMutex();
    if (wifi_events == NULL || wifi_operation_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif initialization failed");
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    if (esp_netif_create_default_wifi_sta() == NULL) {
        return ESP_FAIL;
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "Wi-Fi initialization failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_FLASH), TAG, "Wi-Fi storage setup failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL),
        TAG,
        "Wi-Fi event registration failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL),
        TAG,
        "IP event registration failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "station mode setup failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "Wi-Fi power-save disable failed");

    initialized = true;
    ESP_LOGI(TAG, "Wi-Fi manager ready, power-save disabled for realtime audio");
    return ESP_OK;
}

bool wifi_manager_has_saved_credentials(void)
{
    if (!initialized) {
        return false;
    }

    wifi_config_t config = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &config) != ESP_OK) {
        return false;
    }
    return config.sta.ssid[0] != '\0';
}

esp_err_t wifi_manager_scan(wifi_ap_record_t *records, uint16_t *count)
{
    if (!initialized || records == NULL || count == NULL || *count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(wifi_operation_mutex, portMAX_DELAY);
    connection_requested = false;
    esp_wifi_disconnect();

    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err == ESP_OK) {
        err = esp_wifi_scan_get_ap_records(count, records);
    }
    xSemaphoreGive(wifi_operation_mutex);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "scan complete: %u networks", *count);
    } else {
        ESP_LOGE(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t connect_with_config_unlocked(wifi_config_t *config)
{
    xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
    connection_requested = false;
    esp_wifi_disconnect();

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, config), TAG, "could not save Wi-Fi config");

    retry_count = 0;
    connection_requested = true;
    rti_app_set_state(RTI_APP_STATE_WIFI_CONNECTING);
    ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "connection start failed");

    const EventBits_t bits = xEventGroupWaitBits(
        wifi_events,
        WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

    if ((bits & WIFI_CONNECTED_BIT) != 0) {
        rti_app_set_state(RTI_APP_STATE_READY);
        return ESP_OK;
    }

    connection_requested = false;
    rti_app_set_state(RTI_APP_STATE_ERROR);
    ESP_LOGW(TAG, "connection attempt failed or timed out");
    return ESP_FAIL;
}

static esp_err_t connect_with_config(wifi_config_t *config)
{
    xSemaphoreTake(wifi_operation_mutex, portMAX_DELAY);
    esp_err_t err = connect_with_config_unlocked(config);
    xSemaphoreGive(wifi_operation_mutex);
    return err;
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password)
{
    if (!initialized || ssid == NULL || ssid[0] == '\0' || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, ssid, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, password, sizeof(config.sta.password));
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_LOGI(TAG, "connecting to %s", ssid);
    return connect_with_config(&config);
}

esp_err_t wifi_manager_connect_saved(void)
{
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_config_t config = {0};
    ESP_RETURN_ON_ERROR(esp_wifi_get_config(WIFI_IF_STA, &config), TAG, "could not load Wi-Fi config");
    if (config.sta.ssid[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "reconnecting to saved network %s", (char *)config.sta.ssid);
    return connect_with_config(&config);
}

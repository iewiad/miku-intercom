#include "wifi_manager.h"

#include <stdbool.h>
#include <string.h>

#include "app_state.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT BIT1
#define WIFI_MAX_RETRIES 5
#define WIFI_CONNECT_TIMEOUT_MS 20000
#define WIFI_RECOVERY_DELAY_US (5 * 1000 * 1000LL)

static const char *TAG = "wifi_manager";
static EventGroupHandle_t wifi_events;
static SemaphoreHandle_t wifi_operation_mutex;
static bool initialized;
static bool connection_requested;
static int retry_count;
static bool time_sync_started;
/* The operation mutex serializes manual configuration with timer attempts.
 * Event callbacks use only this short lock for connection intent, never I/O. */
static portMUX_TYPE recovery_guard = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t recovery_timer;
static bool recovery_eligible;
static bool recovering;
static uint32_t connection_generation;
static uint32_t recovery_generation;
static int64_t recovery_due_us;

static void clear_connection_intent(void)
{
    portENTER_CRITICAL(&recovery_guard);
    connection_requested = false;
    recovery_eligible = false;
    recovering = false;
    retry_count = 0;
    if (++connection_generation == 0) ++connection_generation;
    recovery_generation = 0;
    recovery_due_us = 0;
    portEXIT_CRITICAL(&recovery_guard);
    if (recovery_timer) (void)esp_timer_stop(recovery_timer);
    xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
}

static void schedule_recovery(uint32_t generation)
{
    portENTER_CRITICAL(&recovery_guard);
    const bool current = connection_requested && recovery_eligible && recovering &&
                         connection_generation == generation;
    if (current) {
        recovery_generation = generation;
        recovery_due_us = esp_timer_get_time() + WIFI_RECOVERY_DELAY_US;
    }
    portEXIT_CRITICAL(&recovery_guard);
    if (!current || !recovery_timer) return;
    /* A concurrent cancellation may leave a harmless pending timer. The callback
     * checks both intent/generation and the current deadline before any action. */
    (void)esp_timer_stop(recovery_timer);
    (void)esp_timer_start_once(recovery_timer, WIFI_RECOVERY_DELAY_US);
}

static void recovery_timer_callback(void *arg)
{
    (void)arg;
    portENTER_CRITICAL(&recovery_guard);
    const uint32_t generation = recovery_generation;
    portEXIT_CRITICAL(&recovery_guard);
    /* Do not block the timer task behind scan or a bounded manual connection.
     * Holding this mutex for esp_wifi_connect prevents a stale timer from racing
     * a new manual configuration between its intent check and the driver call. */
    if (xSemaphoreTake(wifi_operation_mutex, 0) != pdTRUE) {
        schedule_recovery(generation);
        return;
    }
    portENTER_CRITICAL(&recovery_guard);
    const bool current = connection_requested && recovery_eligible && recovering &&
                         generation == connection_generation &&
                         recovery_due_us > 0 && esp_timer_get_time() >= recovery_due_us;
    if (current) recovery_due_us = 0;
    portEXIT_CRITICAL(&recovery_guard);
    if (current) {
        (void)esp_wifi_connect();
    }
    xSemaphoreGive(wifi_operation_mutex);
    /* Also bounds a driver attempt which accepted the request but did not emit
     * another event. GOT_IP/manual cancellation invalidates this rearm. */
    schedule_recovery(generation);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
        portENTER_CRITICAL(&recovery_guard);
        const bool requested = connection_requested;
        const uint32_t generation = connection_generation;
        const bool retry = requested && retry_count < WIFI_MAX_RETRIES;
        const bool recover = requested && recovery_eligible;
        if (retry) ++retry_count;
        if (recover) recovering = true;
        if (requested && !retry && !recover) connection_requested = false;
        portEXIT_CRITICAL(&recovery_guard);
        if (!requested) return;
        if (retry) {
            ESP_LOGW(TAG, "connection lost; retrying saved connection");
            const esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) {
                if (recover) schedule_recovery(generation);
                else {
                    portENTER_CRITICAL(&recovery_guard);
                    const bool failed = connection_requested && connection_generation == generation;
                    if (failed) connection_requested = false;
                    portEXIT_CRITICAL(&recovery_guard);
                    if (failed) xEventGroupSetBits(wifi_events, WIFI_FAILED_BIT);
                }
            }
        } else if (recover) {
            schedule_recovery(generation);
        } else {
            xEventGroupSetBits(wifi_events, WIFI_FAILED_BIT);
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        portENTER_CRITICAL(&recovery_guard);
        const bool requested = connection_requested;
        if (requested) {
            retry_count = 0;
            recovery_eligible = true;
            recovering = false;
            recovery_generation = 0;
            recovery_due_us = 0;
        }
        portEXIT_CRITICAL(&recovery_guard);
        if (!requested) return;
        if (recovery_timer) (void)esp_timer_stop(recovery_timer);
        if (!time_sync_started) {
            const esp_err_t time_err = esp_netif_sntp_start();
            time_sync_started = time_err == ESP_OK;
            if (time_err != ESP_OK) ESP_LOGW(TAG, "network time unavailable; USB time sync remains available");
        }
        ESP_LOGI(TAG, "connected, ip=" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupClearBits(wifi_events, WIFI_FAILED_BIT);
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

    const esp_timer_create_args_t recovery_args = {
        .callback = recovery_timer_callback,
        .name = "wifi_recover",
        .dispatch_method = ESP_TIMER_TASK,
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&recovery_args, &recovery_timer), TAG, "Wi-Fi recovery timer initialization failed");

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

    esp_sntp_config_t time_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    time_config.start = false;
    time_config.wait_for_sync = false;
    ESP_RETURN_ON_ERROR(esp_netif_sntp_init(&time_config), TAG, "network time initialization failed");
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
    clear_connection_intent();
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
    clear_connection_intent();
    esp_wifi_disconnect();

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, config), TAG, "could not save Wi-Fi config");

    portENTER_CRITICAL(&recovery_guard);
    connection_requested = true;
    portEXIT_CRITICAL(&recovery_guard);
    rti_app_set_state(RTI_APP_STATE_WIFI_CONNECTING);
    const esp_err_t start_err = esp_wifi_connect();
    if (start_err != ESP_OK) {
        clear_connection_intent();
        return start_err;
    }

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

    clear_connection_intent();
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

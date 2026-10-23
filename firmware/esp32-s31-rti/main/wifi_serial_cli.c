#include "wifi_serial_cli.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi_types_generic.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/platform_util.h"
#include "sdkconfig.h"
#include "rti_credentials.h"
#include "wifi_manager.h"

#define SERIAL_CLI_LINE_MAX 4096
#define SERIAL_CLI_SCAN_MAX 24

static const char *TAG = "wifi_serial_cli";
static bool started;

static bool hex_decode(const char *hex, uint8_t *out, size_t out_size, size_t *out_len)
{
    const size_t hex_len = strlen(hex);
    if ((hex_len & 1U) != 0 || hex_len / 2 >= out_size) {
        return false;
    }
    for (size_t i = 0; i < hex_len / 2; ++i) {
        unsigned int value;
        if (sscanf(hex + i * 2, "%2x", &value) != 1) {
            return false;
        }
        out[i] = (uint8_t)value;
    }
    out[hex_len / 2] = '\0';
    *out_len = hex_len / 2;
    return true;
}

static void print_hex(const uint8_t *data, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        printf("%02x", data[i]);
    }
}

static void handle_scan(void)
{
    wifi_ap_record_t records[SERIAL_CLI_SCAN_MAX] = {0};
    uint16_t count = SERIAL_CLI_SCAN_MAX;
    const esp_err_t err = wifi_manager_scan(records, &count);
    if (err != ESP_OK) {
        printf("RTI_WIFI_ERROR scan %s\n", esp_err_to_name(err));
        fflush(stdout);
        return;
    }

    for (uint16_t i = 0; i < count; ++i) {
        printf("RTI_WIFI_AP ");
        print_hex(records[i].ssid, strnlen((char *)records[i].ssid, sizeof(records[i].ssid)));
        printf(" %d %u %d\n", records[i].rssi, records[i].primary, records[i].authmode);
    }
    printf("RTI_WIFI_SCAN_DONE %u\n", count);
    fflush(stdout);
}

static void handle_connect(char *arguments)
{
    char *save = NULL;
    char *ssid_hex = strtok_r(arguments, " \r\n", &save);
    char *password_hex = strtok_r(NULL, " \r\n", &save);
    if (ssid_hex == NULL || password_hex == NULL) {
        printf("RTI_WIFI_ERROR connect invalid_arguments\n");
        fflush(stdout);
        return;
    }

    uint8_t ssid[33];
    uint8_t password[65];
    size_t ssid_len = 0;
    size_t password_len = 0;
    const bool empty_password = strcmp(password_hex, "-") == 0;
    if (!hex_decode(ssid_hex, ssid, sizeof(ssid), &ssid_len) || ssid_len == 0 ||
        (!empty_password && !hex_decode(password_hex, password, sizeof(password), &password_len))) {
        printf("RTI_WIFI_ERROR connect invalid_encoding\n");
        fflush(stdout);
        return;
    }
    if (empty_password) {
        password[0] = '\0';
    }

    printf("RTI_WIFI_CONNECTING ");
    print_hex(ssid, ssid_len);
    printf("\n");
    fflush(stdout);

    const esp_err_t err = wifi_manager_connect((char *)ssid, (char *)password);
    if (err == ESP_OK) {
        printf("RTI_WIFI_CONNECTED ");
        print_hex(ssid, ssid_len);
        printf("\n");
    } else {
        printf("RTI_WIFI_ERROR connect %s\n", esp_err_to_name(err));
    }
    fflush(stdout);
    mbedtls_platform_zeroize(password, sizeof(password));
}

static void handle_rti_config(char *arguments)
{
    char *save = NULL;
    char *app_id_hex = strtok_r(arguments, " \r\n", &save);
    char *token_hex = strtok_r(NULL, " \r\n", &save);
    uint8_t app_id[RTI_APP_ID_MAX + 1];
    uint8_t api_token[RTI_API_TOKEN_MAX + 1];
    size_t app_id_len = 0;
    size_t token_len = 0;

    if (app_id_hex == NULL || token_hex == NULL ||
        !hex_decode(app_id_hex, app_id, sizeof(app_id), &app_id_len) || app_id_len == 0 ||
        !hex_decode(token_hex, api_token, sizeof(api_token), &token_len) || token_len == 0) {
        printf("RTI_CONFIG_ERROR invalid_encoding\n");
        fflush(stdout);
        mbedtls_platform_zeroize(api_token, sizeof(api_token));
        return;
    }

    const esp_err_t err = rti_credentials_save((char *)app_id, (char *)api_token);
    mbedtls_platform_zeroize(api_token, sizeof(api_token));
    if (err == ESP_OK) {
        printf("RTI_CONFIG_SAVED\n");
    } else {
        printf("RTI_CONFIG_ERROR %s\n", esp_err_to_name(err));
    }
    fflush(stdout);
}

static void serial_cli_task(void *arg)
{
    (void)arg;
    char *line = malloc(SERIAL_CLI_LINE_MAX);
    if (line == NULL) {
        started = false;
        ESP_LOGE(TAG, "could not allocate serial command buffer");
        vTaskDelete(NULL);
    }
    printf("RTI_WIFI_READY\n");
    fflush(stdout);

    while (fgets(line, SERIAL_CLI_LINE_MAX, stdin) != NULL) {
        if (strncmp(line, "RTI_WIFI_PING", strlen("RTI_WIFI_PING")) == 0) {
            printf("RTI_WIFI_PONG\n");
            fflush(stdout);
        } else if (strncmp(line, "RTI_WIFI_SCAN", strlen("RTI_WIFI_SCAN")) == 0) {
            handle_scan();
        } else if (strncmp(line, "RTI_WIFI_CONNECT ", strlen("RTI_WIFI_CONNECT ")) == 0) {
            handle_connect(line + strlen("RTI_WIFI_CONNECT "));
        } else if (strncmp(line, "RTI_CONFIG_SET ", strlen("RTI_CONFIG_SET ")) == 0) {
            handle_rti_config(line + strlen("RTI_CONFIG_SET "));
        } else if (strncmp(line, "RTI_CONFIG_STATUS", strlen("RTI_CONFIG_STATUS")) == 0) {
            printf("RTI_CONFIG_STATUS %d\n", rti_credentials_are_configured() ? 1 : 0);
            fflush(stdout);
        } else if (strncmp(line, "RTI_CONFIG_CLEAR", strlen("RTI_CONFIG_CLEAR")) == 0) {
            const esp_err_t err = rti_credentials_clear();
            if (err == ESP_OK) {
                printf("RTI_CONFIG_CLEARED\n");
            } else {
                printf("RTI_CONFIG_ERROR %s\n", esp_err_to_name(err));
            }
            fflush(stdout);
        } else if (strncmp(line, "RTI_DEVICE_RESTART", strlen("RTI_DEVICE_RESTART")) == 0) {
            printf("RTI_DEVICE_RESTARTING\n");
            fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
    }

    free(line);
    started = false;
    ESP_LOGW(TAG, "serial input closed");
    vTaskDelete(NULL);
}

esp_err_t wifi_serial_cli_start(void)
{
    if (started) {
        return ESP_OK;
    }
    if (!uart_is_driver_installed(CONFIG_ESP_CONSOLE_UART_NUM)) {
        const esp_err_t err = uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 8 * 1024, 0, 0, NULL, 0);
        if (err != ESP_OK) {
            return err;
        }
    }
    uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
    if (xTaskCreate(serial_cli_task, "wifi_serial_cli", 6 * 1024, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    started = true;
    ESP_LOGI(TAG, "USB serial Wi-Fi provisioning ready");
    return ESP_OK;
}

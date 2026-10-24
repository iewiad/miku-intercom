#include "wifi_serial_cli.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

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
#include "group_config.h"
#include "group_controller.h"
#include "wifi_manager.h"

#define SERIAL_CLI_LINE_MAX 4096
#define SERIAL_CLI_SCAN_MAX 24

static const char *TAG = "wifi_serial_cli";
static bool started;

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool hex_decode(const char *hex, uint8_t *out, size_t out_size, size_t *out_len)
{
    const size_t hex_len = strlen(hex);
    if ((hex_len & 1U) != 0 || hex_len / 2 >= out_size) {
        return false;
    }
    for (size_t i = 0; i < hex_len / 2; ++i) {
        const int high = hex_nibble(hex[i * 2]);
        const int low = hex_nibble(hex[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        out[i] = (uint8_t)((high << 4) | low);
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

static void handle_intercom_config(const char *hex)
{
    uint8_t *json = calloc(1, GROUP_CONFIG_JSON_MAX + 1);
    if (!json) {
        printf("INTERCOM_CONFIG_ERROR out_of_memory\n");
        return;
    }
    size_t size = 0;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (hex_decode(hex, json, GROUP_CONFIG_JSON_MAX + 1, &size) && size > 0) {
        err = group_config_store_json((const char *)json, size);
    }
    mbedtls_platform_zeroize(json, GROUP_CONFIG_JSON_MAX + 1);
    free(json);
    printf(err == ESP_OK ? "INTERCOM_CONFIG_SAVED\n" : "INTERCOM_CONFIG_ERROR invalid_or_unavailable\n");
}

static void handle_intercom_time(const char *seconds)
{
    bool digits = seconds[0] != '\0';
    for (const char *c = seconds; *c; ++c) {
        if (*c < '0' || *c > '9') digits = false;
    }
    errno = 0;
    char *end = NULL;
    const long long epoch = strtoll(seconds, &end, 10);
    if (!digits || errno || !end || *end || epoch < 1735689600LL || epoch > 4102444800LL) {
        printf("INTERCOM_TIME_ERROR invalid_time\n");
        return;
    }
    const struct timeval tv = {.tv_sec = (time_t)epoch, .tv_usec = 0};
    printf(settimeofday(&tv, NULL) == 0 ? "INTERCOM_TIME_SET\n" : "INTERCOM_TIME_ERROR unavailable\n");
}

static bool parse_intercom_switch(const char *arguments, uint32_t *generation, char group_id[81])
{
    uint32_t value = 0;
    size_t digits = 0;
    const char *cursor = arguments;
    while (*cursor >= '0' && *cursor <= '9') {
        const uint32_t digit = (uint32_t)(*cursor - '0');
        if (++digits > 10 || value > (UINT32_MAX - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
        ++cursor;
    }
    if (digits == 0 || *cursor++ != ' ') {
        return false;
    }
    const char *id = cursor;
    size_t length = 0;
    for (; *cursor; ++cursor) {
        const char c = *cursor;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_') || ++length > 80) {
            return false;
        }
    }
    if (length < 8) {
        return false;
    }
    memcpy(group_id, id, length);
    group_id[length] = '\0';
    *generation = value;
    return true;
}

/* Handle only the new group commands before the legacy line normalizer. This
 * keeps embedded CR / trailing fields from being silently discarded. The
 * controller queues work; the serial task never performs group HTTP itself. */
static bool handle_intercom_group_command(const char *line)
{
    const bool groups = strncmp(line, "INTERCOM_GROUPS", strlen("INTERCOM_GROUPS")) == 0;
    const bool switching = strncmp(line, "INTERCOM_SWITCH", strlen("INTERCOM_SWITCH")) == 0;
    if (!groups && !switching) {
        return false;
    }
    size_t length = strlen(line);
    if (length && line[length - 1] == '\n') {
        --length;
        if (length && line[length - 1] == '\r') {
            --length;
        }
    }
    char command[sizeof("INTERCOM_SWITCH ") + 10 + 1 + 80];
    if (length >= sizeof(command)) {
        printf("%s\n", groups ? "INTERCOM_GROUPS_ERROR invalid_request" : "INTERCOM_SWITCH_ERROR invalid_request");
        return true;
    }
    memcpy(command, line, length);
    command[length] = '\0';
    if (groups) {
        if (strcmp(command, "INTERCOM_GROUPS") == 0) {
            group_controller_print_groups();
        } else {
            printf("INTERCOM_GROUPS_ERROR invalid_request\n");
        }
        return true;
    }
    uint32_t generation = 0;
    char group_id[81];
    const size_t prefix = strlen("INTERCOM_SWITCH");
    if (command[prefix] != ' ' || !parse_intercom_switch(command + prefix + 1, &generation, group_id)) {
        printf("INTERCOM_SWITCH_ERROR invalid_request\n");
        return true;
    }
    const esp_err_t err = group_controller_switch_group(group_id, generation, NULL);
    if (err == ESP_OK) {
        printf("INTERCOM_SWITCH_REQUESTED\n");
    } else if (err == ESP_ERR_INVALID_ARG) {
        printf("INTERCOM_SWITCH_ERROR invalid_request\n");
    } else if (err == ESP_ERR_INVALID_STATE) {
        printf("INTERCOM_SWITCH_ERROR invalid_state\n");
    } else {
        printf("INTERCOM_SWITCH_ERROR unavailable\n");
    }
    return true;
}

static void serial_cli_task(void *arg)
{
    (void)arg;
    char *line = malloc(SERIAL_CLI_LINE_MAX);
    if (line == NULL) {
        started = false;
        ESP_LOGE(TAG, "could not allocate serial command buffer");
        vTaskDelete(NULL);
        return;
    }
    printf("RTI_WIFI_READY\n");
    fflush(stdout);

    while (fgets(line, SERIAL_CLI_LINE_MAX, stdin) != NULL) {
        if (strchr(line, '\n') == NULL) {
            int ch;
            while ((ch = fgetc(stdin)) != '\n' && ch != EOF) { }
            mbedtls_platform_zeroize(line, SERIAL_CLI_LINE_MAX);
            printf("INTERCOM_ERROR command_too_long\n");
            fflush(stdout);
            continue;
        }
        if (handle_intercom_group_command(line)) {
            fflush(stdout);
            mbedtls_platform_zeroize(line, SERIAL_CLI_LINE_MAX);
            continue;
        }
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "INTERCOM_CONFIG_SET ", strlen("INTERCOM_CONFIG_SET ")) == 0) {
            handle_intercom_config(line + strlen("INTERCOM_CONFIG_SET "));
        } else if (strcmp(line, "INTERCOM_CONFIG_STATUS") == 0) {
            printf("INTERCOM_CONFIG_STATUS %d\n", group_config_ready() ? 1 : 0);
        } else if (strncmp(line, "INTERCOM_TIME_SET ", strlen("INTERCOM_TIME_SET ")) == 0) {
            handle_intercom_time(line + strlen("INTERCOM_TIME_SET "));
        } else if (strcmp(line, "INTERCOM_START") == 0) {
            group_controller_action(false, NULL);
            printf("INTERCOM_START_REQUESTED\n");
        } else if (strcmp(line, "INTERCOM_STOP") == 0) {
            group_controller_action(true, NULL);
            printf("INTERCOM_STOP_REQUESTED\n");
        } else if (strcmp(line, "INTERCOM_PTT_DOWN") == 0) {
            group_controller_ptt(true, NULL);
            printf("INTERCOM_PTT_DOWN_REQUESTED\n");
        } else if (strcmp(line, "INTERCOM_PTT_UP") == 0) {
            group_controller_ptt(false, NULL);
            printf("INTERCOM_PTT_UP_REQUESTED\n");
        } else if (strncmp(line, "INTERCOM_JOIN ", strlen("INTERCOM_JOIN ")) == 0) {
            const esp_err_t err = group_controller_join_group(line + strlen("INTERCOM_JOIN "), NULL);
            printf(err == ESP_OK ? "INTERCOM_JOIN_REQUESTED\n" : "INTERCOM_JOIN_ERROR invalid_or_unavailable\n");
        } else if (strcmp(line, "INTERCOM_VOICE_START") == 0 || strcmp(line, "INTERCOM_VOICE_CANCEL") == 0) {
            const bool cancel = strcmp(line, "INTERCOM_VOICE_CANCEL") == 0;
            const esp_err_t err = group_controller_voice_action(cancel, NULL);
            printf(err == ESP_OK ? "INTERCOM_VOICE_REQUESTED\n" : "INTERCOM_VOICE_ERROR invalid_state\n");
        } else if (strcmp(line, "INTERCOM_STATUS") == 0) {
            group_controller_print_status();
        } else if (strncmp(line, "RTI_WIFI_PING", strlen("RTI_WIFI_PING")) == 0) {
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
            mbedtls_platform_zeroize(line, SERIAL_CLI_LINE_MAX);
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
        fflush(stdout);
        mbedtls_platform_zeroize(line, SERIAL_CLI_LINE_MAX);
    }

    mbedtls_platform_zeroize(line, SERIAL_CLI_LINE_MAX);
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

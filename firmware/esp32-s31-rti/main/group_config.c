#include "group_config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/x509_crt.h"
#include "nvs.h"

#define GROUP_NAMESPACE "intercom"
#define GROUP_CONFIG_KEY "config_v1"

static bool bounded(const char *value, size_t capacity)
{
    return value[0] != '\0' && strnlen(value, capacity) < capacity;
}

static bool identifier(const char *value)
{
    for (; *value; ++value) {
        const unsigned char c = (unsigned char)*value;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

static bool private_server_url(const char *url)
{
    if (strncmp(url, "https://", 8) != 0) {
        return false;
    }
    unsigned a, b, c, d, port;
    int consumed = 0;
    if (sscanf(url + 8, "%u.%u.%u.%u:%u%n", &a, &b, &c, &d, &port, &consumed) != 5 ||
        url[8 + consumed] != '\0' || a > 255 || b > 255 || c > 255 || d > 255 ||
        port == 0 || port > 65535 || !(a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168))) {
        return false;
    }
    char canonical[48];
    snprintf(canonical, sizeof(canonical), "https://%u.%u.%u.%u:%u", a, b, c, d, port);
    return strcmp(url, canonical) == 0;
}

static bool valid_ca(const char *pem)
{
    static const char begin[] = "-----BEGIN CERTIFICATE-----";
    static const char end[] = "-----END CERTIFICATE-----";
    const size_t size = strlen(pem);
    size_t trimmed = size;
    while (trimmed && (pem[trimmed - 1] == '\r' || pem[trimmed - 1] == '\n')) {
        --trimmed;
    }
    if (strncmp(pem, begin, sizeof(begin) - 1) != 0 || trimmed < sizeof(end) - 1 ||
        memcmp(pem + trimmed - (sizeof(end) - 1), end, sizeof(end) - 1) != 0 ||
        strstr(pem, "PRIVATE KEY") != NULL) {
        return false;
    }
    for (size_t i = 0; i < size; ++i) {
        const unsigned char ch = (unsigned char)pem[i];
        if (!(ch == '\r' || ch == '\n' || ch == '-' || ch == ' ' || ch == '+' || ch == '/' || ch == '=' ||
              (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))) {
            return false;
        }
    }
    mbedtls_x509_crt certificate;
    mbedtls_x509_crt_init(&certificate);
    bool valid = mbedtls_x509_crt_parse(&certificate, (const unsigned char *)pem, size + 1) == 0;
    for (const mbedtls_x509_crt *item = &certificate; valid && item; item = item->next) {
        valid = item->ca_istrue != 0;
    }
    mbedtls_x509_crt_free(&certificate);
    return valid;
}

static bool valid_config(const group_config_t *config)
{
    return bounded(config->server_url, sizeof(config->server_url)) &&
           bounded(config->ca_pem, sizeof(config->ca_pem)) &&
           bounded(config->device_id, sizeof(config->device_id)) &&
           bounded(config->token, sizeof(config->token)) &&
           strnlen(config->group_code, sizeof(config->group_code)) < sizeof(config->group_code) &&
           private_server_url(config->server_url) && identifier(config->device_id) &&
           identifier(config->token) && identifier(config->group_code) && valid_ca(config->ca_pem);
}

esp_err_t group_config_load(group_config_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    nvs_handle_t handle;
    esp_err_t err = nvs_open(GROUP_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    size_t size = sizeof(*out);
    err = nvs_get_blob(handle, GROUP_CONFIG_KEY, out, &size);
    nvs_close(handle);
    if (err == ESP_OK && (size != sizeof(*out) || !valid_config(out))) {
        err = ESP_ERR_INVALID_ARG;
    }
    if (err != ESP_OK) {
        mbedtls_platform_zeroize(out, sizeof(*out));
    }
    return err;
}

bool group_config_ready(void)
{
    group_config_t *config = calloc(1, sizeof(*config));
    if (!config) {
        return false;
    }
    const bool ready = group_config_load(config) == ESP_OK;
    mbedtls_platform_zeroize(config, sizeof(*config));
    free(config);
    return ready;
}

static void zero_json_strings(cJSON *item)
{
    for (; item; item = item->next) {
        if (cJSON_IsString(item) && item->valuestring) {
            mbedtls_platform_zeroize(item->valuestring, strlen(item->valuestring));
        }
        zero_json_strings(item->child);
    }
}

esp_err_t group_config_store_json(const char *json, size_t size)
{
    if (!json || size == 0 || size > GROUP_CONFIG_JSON_MAX || memchr(json, '\0', size)) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Only a flat object is documented; reject nested containers before the
     * parser to keep even malformed local input from consuming deep stacks. */
    bool quoted = false;
    unsigned depth = 0;
    for (size_t i = 0; i < size; ++i) {
        if (quoted && json[i] == '\\') {
            if (++i >= size) return ESP_ERR_INVALID_ARG;
            continue;
        }
        if (json[i] == '"') {
            quoted = !quoted;
        } else if (!quoted) {
            if (json[i] == '[' || (json[i] == '{' && ++depth > 1)) return ESP_ERR_INVALID_ARG;
            if (json[i] == '}' && (depth == 0 || --depth > 0)) return ESP_ERR_INVALID_ARG;
        }
    }
    if (quoted || depth) return ESP_ERR_INVALID_ARG;
    /* Reject decoded NULs before cJSON can truncate a credential string. */
    for (size_t i = 0; i < size; ++i) {
        if (json[i] == '\\' && i + 1 < size) {
            if (i + 5 < size && json[i + 1] == 'u' && memcmp(json + i + 2, "0000", 4) == 0) {
                return ESP_ERR_INVALID_ARG;
            }
            ++i;
        }
    }
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, size, &end, false);
    group_config_t *config = calloc(1, sizeof(*config));
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (!config) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    if (!cJSON_IsObject(root) || !end) {
        goto done;
    }
    while ((size_t)(end - json) < size && isspace((unsigned char)*end)) {
        ++end;
    }
    if ((size_t)(end - json) != size) {
        goto done;
    }
    unsigned fields = 0;
    for (cJSON *field = root->child; field; field = field->next) {
        char *destination = NULL;
        size_t capacity = 0;
        unsigned bit = 0;
#define FIELD(name, flag) if (strcmp(field->string, #name) == 0) { destination = config->name; capacity = sizeof(config->name); bit = flag; }
        if (!field->string || !cJSON_IsString(field) || !field->valuestring) {
            goto done;
        }
        FIELD(server_url, 1)
        else FIELD(ca_pem, 2)
        else FIELD(device_id, 4)
        else FIELD(token, 8)
        else FIELD(group_code, 16)
#undef FIELD
        if (!destination || (fields & bit) || (bit == 16 ? strlen(field->valuestring) >= capacity : !bounded(field->valuestring, capacity))) {
            goto done;
        }
        fields |= bit;
        memcpy(destination, field->valuestring, strlen(field->valuestring) + 1);
    }
    if ((fields != 15 && fields != 31) || !valid_config(config)) {
        goto done;
    }
    nvs_handle_t handle;
    err = nvs_open(GROUP_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, GROUP_CONFIG_KEY, config, sizeof(*config));
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
done:
    if (root) {
        zero_json_strings(root);
        cJSON_Delete(root);
    }
    if (config) {
        mbedtls_platform_zeroize(config, sizeof(*config));
        free(config);
    }
    return err;
}

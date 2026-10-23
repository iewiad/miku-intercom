#include "rti_credentials.h"

#include <string.h>

#include "mbedtls/platform_util.h"
#include "nvs.h"

#define RTI_NVS_NAMESPACE "rti_auth"
#define RTI_NVS_APP_ID_KEY "app_id"
#define RTI_NVS_TOKEN_KEY "api_token"

esp_err_t rti_credentials_load(rti_credentials_t *credentials)
{
    if (credentials == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(credentials, 0, sizeof(*credentials));
    nvs_handle_t handle;
    esp_err_t err = nvs_open(RTI_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t app_id_size = sizeof(credentials->app_id);
    size_t token_size = sizeof(credentials->api_token);
    err = nvs_get_str(handle, RTI_NVS_APP_ID_KEY, credentials->app_id, &app_id_size);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, RTI_NVS_TOKEN_KEY, credentials->api_token, &token_size);
    }
    nvs_close(handle);
    if (err != ESP_OK || credentials->app_id[0] == '\0' || credentials->api_token[0] == '\0') {
        mbedtls_platform_zeroize(credentials, sizeof(*credentials));
        return err == ESP_OK ? ESP_ERR_NOT_FOUND : err;
    }
    return ESP_OK;
}

esp_err_t rti_credentials_save(const char *app_id, const char *api_token)
{
    if (app_id == NULL || api_token == NULL || app_id[0] == '\0' || api_token[0] == '\0' ||
        strlen(app_id) > RTI_APP_ID_MAX || strlen(api_token) > RTI_API_TOKEN_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(RTI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, RTI_NVS_APP_ID_KEY, app_id);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, RTI_NVS_TOKEN_KEY, api_token);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t rti_credentials_clear(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(RTI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

bool rti_credentials_are_configured(void)
{
    rti_credentials_t credentials;
    const bool configured = rti_credentials_load(&credentials) == ESP_OK;
    mbedtls_platform_zeroize(&credentials, sizeof(credentials));
    return configured;
}

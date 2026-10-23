#include "rti_session.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "app_state.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"
#include "rti_credentials.h"

#define RTI_HTTP_RESPONSE_MAX (8 * 1024)
#define RTI_RTC_RESPONSE_MAX (24 * 1024)
#define RTI_HTTP_TIMEOUT_MS 20000
#define RTI_VOICE_PROFILE_MAX 31
#define RTI_SETTINGS_NAMESPACE "rti_settings"
#define RTI_SETTINGS_VOICE_KEY "voice"
#define RTI_SETTINGS_VOICE_TITLE_KEY "voice_title"

static const char *TAG = "rti_session";
static char selected_voice_profile[RTI_VOICE_PROFILE_MAX + 1];
static char selected_voice_title[RTI_VOICE_PROFILE_TITLE_MAX];
static bool selected_voice_loaded;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    bool overflowed;
} http_response_buffer_t;

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || event->user_data == NULL || event->data_len <= 0) {
        return ESP_OK;
    }

    http_response_buffer_t *response = event->user_data;
    if (response->length + (size_t)event->data_len >= response->capacity) {
        response->overflowed = true;
        return ESP_ERR_NO_MEM;
    }
    memcpy(response->data + response->length, event->data, event->data_len);
    response->length += event->data_len;
    response->data[response->length] = '\0';
    return ESP_OK;
}

const char *rti_session_get_voice_profile(void)
{
    if (!selected_voice_loaded) {
        strlcpy(selected_voice_profile, CONFIG_MIKU_RTI_VOICE_PROFILE,
                sizeof(selected_voice_profile));
        nvs_handle_t handle;
        if (nvs_open(RTI_SETTINGS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
            size_t size = sizeof(selected_voice_profile);
            if (nvs_get_str(handle, RTI_SETTINGS_VOICE_KEY, selected_voice_profile, &size) != ESP_OK ||
                selected_voice_profile[0] == '\0') {
                strlcpy(selected_voice_profile, CONFIG_MIKU_RTI_VOICE_PROFILE,
                        sizeof(selected_voice_profile));
            }
            size = sizeof(selected_voice_title);
            if (nvs_get_str(handle, RTI_SETTINGS_VOICE_TITLE_KEY, selected_voice_title, &size) != ESP_OK ||
                selected_voice_title[0] == '\0') {
                strlcpy(selected_voice_title, selected_voice_profile, sizeof(selected_voice_title));
            }
            nvs_close(handle);
        } else {
            strlcpy(selected_voice_title, selected_voice_profile, sizeof(selected_voice_title));
        }
        selected_voice_loaded = true;
    }
    return selected_voice_profile;
}

const char *rti_session_get_voice_title(void)
{
    (void)rti_session_get_voice_profile();
    return selected_voice_title;
}

esp_err_t rti_session_set_voice_profile(const char *voice_profile, const char *voice_title)
{
    if (voice_profile == NULL || voice_profile[0] == '\0' ||
        strlen(voice_profile) > RTI_VOICE_PROFILE_MAX || voice_title == NULL ||
        voice_title[0] == '\0' || strlen(voice_title) >= RTI_VOICE_PROFILE_TITLE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(RTI_SETTINGS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, RTI_SETTINGS_VOICE_KEY, voice_profile);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, RTI_SETTINGS_VOICE_TITLE_KEY, voice_title);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err == ESP_OK) {
        strlcpy(selected_voice_profile, voice_profile, sizeof(selected_voice_profile));
        strlcpy(selected_voice_title, voice_title, sizeof(selected_voice_title));
        selected_voice_loaded = true;
        ESP_LOGI(TAG, "voice profile selected: %s (%s)", selected_voice_title,
                 selected_voice_profile);
    }
    return err;
}

static cJSON *build_create_request(const rti_credentials_t *credentials)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *metadata = cJSON_CreateObject();
    if (root == NULL || metadata == NULL ||
        !cJSON_AddStringToObject(root, "app_id", credentials->app_id) ||
        !cJSON_AddStringToObject(root, "user_id", CONFIG_MIKU_RTI_DEVICE_ID) ||
        !cJSON_AddStringToObject(root, "client_id", CONFIG_MIKU_RTI_DEVICE_ID) ||
        !cJSON_AddStringToObject(root, "model_profile", CONFIG_MIKU_RTI_MODEL_PROFILE) ||
        !cJSON_AddStringToObject(root, "role_profile", CONFIG_MIKU_RTI_ROLE_PROFILE) ||
        !cJSON_AddStringToObject(root, "voice_profile", rti_session_get_voice_profile()) ||
        !cJSON_AddStringToObject(root, "scenario", "esp32_s31_voice_device") ||
        !cJSON_AddStringToObject(root, "client_transport", "platform_rtc") ||
        !cJSON_AddStringToObject(root, "region", CONFIG_MIKU_RTI_REGION) ||
        !cJSON_AddStringToObject(metadata, "source", "esp32-s31-korvo-1")) {
        cJSON_Delete(root);
        cJSON_Delete(metadata);
        return NULL;
    }
    cJSON_AddItemToObject(root, "metadata", metadata);

#if CONFIG_MIKU_RTI_ENABLE_WEB_SEARCH
    cJSON *realtime_tools = cJSON_CreateObject();
    cJSON *web_search = cJSON_CreateObject();
    cJSON *provider_options = cJSON_CreateObject();
    if (realtime_tools == NULL || web_search == NULL || provider_options == NULL ||
        !cJSON_AddBoolToObject(root, "enable_search", true) ||
        !cJSON_AddBoolToObject(web_search, "enabled", true) ||
        !cJSON_AddStringToObject(provider_options, "provider", CONFIG_MIKU_RTI_WEB_SEARCH_PROVIDER)) {
        cJSON_Delete(realtime_tools);
        cJSON_Delete(web_search);
        cJSON_Delete(provider_options);
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddItemToObject(web_search, "provider_options", provider_options);
    cJSON_AddItemToObject(realtime_tools, "web_search", web_search);
    cJSON_AddItemToObject(root, "realtime_tools", realtime_tools);
#endif

    return root;
}

static bool copy_json_string(const cJSON *object, const char *name, char *destination, size_t destination_size)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(value) || value->valuestring == NULL) {
        return false;
    }
    const size_t length = strlen(value->valuestring);
    if (length >= destination_size) {
        ESP_LOGE(TAG, "session response field too long: %s bytes=%u capacity=%u",
                 name, (unsigned)length, (unsigned)destination_size);
        return false;
    }
    strlcpy(destination, value->valuestring, destination_size);
    return true;
}

static esp_err_t parse_create_response(const char *json, rti_session_t *session)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *endpoint = cJSON_GetObjectItemCaseSensitive(root, "client_endpoint");
    const cJSON *expires_at = cJSON_IsObject(endpoint)
                                  ? cJSON_GetObjectItemCaseSensitive(endpoint, "expires_at_ms")
                                  : NULL;
    char endpoint_url[384] = {0};
    const bool valid = copy_json_string(root, "session_id", session->session_id, sizeof(session->session_id)) &&
                       copy_json_string(root, "trace_id", session->trace_id, sizeof(session->trace_id)) &&
                       cJSON_IsObject(endpoint) &&
                       copy_json_string(endpoint, "url", endpoint_url, sizeof(endpoint_url)) &&
                       copy_json_string(endpoint, "access_token", session->access_token, sizeof(session->access_token)) &&
                       cJSON_IsNumber(expires_at);
    if (valid) {
        session->expires_at_ms = (int64_t)expires_at->valuedouble;
        if (strncmp(endpoint_url, "https://", 8) == 0 || strncmp(endpoint_url, "http://", 7) == 0) {
            strlcpy(session->rtc_url, endpoint_url, sizeof(session->rtc_url));
        } else {
            const size_t base_length = strlen(CONFIG_MIKU_RTI_DEVICE_API_BASE_URL);
            const bool base_has_slash = base_length > 0 && CONFIG_MIKU_RTI_DEVICE_API_BASE_URL[base_length - 1] == '/';
            const bool endpoint_has_slash = endpoint_url[0] == '/';
            snprintf(session->rtc_url, sizeof(session->rtc_url), "%s%s%s",
                     CONFIG_MIKU_RTI_DEVICE_API_BASE_URL,
                     base_has_slash == endpoint_has_slash ? (base_has_slash ? "" : "/") : "",
                     base_has_slash && endpoint_has_slash ? endpoint_url + 1 : endpoint_url);
        }
    }
    cJSON_Delete(root);
    return valid ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static void log_api_error(const char *operation, int status, const char *json)
{
    cJSON *root = cJSON_Parse(json);
    const cJSON *code = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "code");
    if (cJSON_IsString(code) && code->valuestring != NULL) {
        ESP_LOGE(TAG, "%s rejected: HTTP %d code=%s", operation, status, code->valuestring);
    } else {
        ESP_LOGE(TAG, "%s rejected: HTTP %d", operation, status);
    }
    cJSON_Delete(root);
}

static bool voice_profile_exists(const rti_voice_profile_list_t *profiles, const char *name)
{
    for (size_t i = 0; i < profiles->count; ++i) {
        if (strcmp(profiles->names[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static void append_voice_profile(rti_voice_profile_list_t *profiles, const char *name,
                                 const char *title)
{
    if (name == NULL || name[0] == '\0' ||
        strlen(name) >= RTI_VOICE_PROFILE_NAME_MAX ||
        profiles->count >= RTI_VOICE_PROFILE_LIMIT ||
        voice_profile_exists(profiles, name)) {
        return;
    }
    const size_t index = profiles->count++;
    strlcpy(profiles->names[index], name, RTI_VOICE_PROFILE_NAME_MAX);
    strlcpy(profiles->titles[index],
            title != NULL && title[0] != '\0' ? title : name,
            RTI_VOICE_PROFILE_TITLE_MAX);
}

static esp_err_t parse_voice_profiles(const char *json, rti_voice_profile_list_t *profiles)
{
    cJSON *root = cJSON_Parse(json);
    const cJSON *items = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "profiles");
    if (!cJSON_IsArray(items)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *profile = NULL;
    cJSON_ArrayForEach(profile, items) {
        const cJSON *details = cJSON_GetObjectItemCaseSensitive(profile, "voice_profile_details");
        const cJSON *detail = NULL;
        cJSON_ArrayForEach(detail, details) {
            const cJSON *voice_profile = cJSON_GetObjectItemCaseSensitive(detail, "voice_profile");
            const cJSON *title = cJSON_GetObjectItemCaseSensitive(detail, "name");
            if (cJSON_IsString(voice_profile) && voice_profile->valuestring != NULL) {
                append_voice_profile(profiles, voice_profile->valuestring,
                                     cJSON_IsString(title) ? title->valuestring : NULL);
            }
        }
        const cJSON *voices = cJSON_GetObjectItemCaseSensitive(profile, "voice_profiles");
        const cJSON *voice = NULL;
        cJSON_ArrayForEach(voice, voices) {
            if (cJSON_IsString(voice) && voice->valuestring != NULL) {
                append_voice_profile(profiles, voice->valuestring, voice->valuestring);
            }
        }
    }
    cJSON_Delete(root);
    return profiles->count > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t rti_session_list_voice_profiles(rti_voice_profile_list_t *profiles)
{
    if (profiles == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(profiles, 0, sizeof(*profiles));

    rti_credentials_t credentials;
    esp_err_t err = rti_credentials_load(&credentials);
    if (err != ESP_OK) {
        return err;
    }

    char *authorization = malloc(strlen(credentials.api_token) + strlen("Bearer ") + 1);
    if (authorization != NULL) {
        authorization[0] = '\0';
    }
    http_response_buffer_t response = {
        .data = calloc(1, RTI_HTTP_RESPONSE_MAX),
        .capacity = RTI_HTTP_RESPONSE_MAX,
    };
    char url[384];
    const size_t base_length = strlen(CONFIG_MIKU_RTI_DEVICE_API_BASE_URL);
    const int url_length = snprintf(url, sizeof(url), "%s%srtic/v1/realtime/profiles",
                                    CONFIG_MIKU_RTI_DEVICE_API_BASE_URL,
                                    base_length > 0 && CONFIG_MIKU_RTI_DEVICE_API_BASE_URL[base_length - 1] == '/' ? "" : "/");
    if (authorization == NULL || response.data == NULL || url_length < 0 ||
        (size_t)url_length >= sizeof(url)) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    snprintf(authorization, strlen(credentials.api_token) + strlen("Bearer ") + 1,
             "Bearer %s", credentials.api_token);

    const esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &response,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = RTI_HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_http_client_set_header(client, "Authorization", authorization);
    err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK && status <= 0) {
        ESP_LOGE(TAG, "profile list failed: %s", esp_err_to_name(err));
        goto cleanup;
    }
    if (response.overflowed) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    if (status < 200 || status >= 300) {
        log_api_error("profile list", status, response.data);
        err = ESP_FAIL;
        goto cleanup;
    }
    err = parse_voice_profiles(response.data, profiles);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "loaded %u voice profiles from RTIC", (unsigned)profiles->count);
    }

cleanup:
    if (authorization != NULL) {
        mbedtls_platform_zeroize(authorization, strlen(authorization));
        free(authorization);
    }
    mbedtls_platform_zeroize(&credentials, sizeof(credentials));
    free(response.data);
    if (err != ESP_OK) {
        memset(profiles, 0, sizeof(*profiles));
    }
    return err;
}

esp_err_t rti_session_create(rti_session_t *session)
{
    if (session == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(session, 0, sizeof(*session));
    bool stop_invalid_session = false;

    rti_credentials_t credentials;
    esp_err_t err = rti_credentials_load(&credentials);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RTI credentials are not provisioned; use the USB configuration tool");
        return err;
    }

    rti_app_set_state(RTI_APP_STATE_SESSION_CREATING);
    cJSON *request_json = build_create_request(&credentials);
    char *request_body = request_json == NULL ? NULL : cJSON_PrintUnformatted(request_json);
    cJSON_Delete(request_json);
    if (request_body == NULL) {
        mbedtls_platform_zeroize(&credentials, sizeof(credentials));
        rti_app_set_state(RTI_APP_STATE_ERROR);
        return ESP_ERR_NO_MEM;
    }

    char *authorization = malloc(strlen(credentials.api_token) + strlen("Bearer ") + 1);
    if (authorization != NULL) {
        authorization[0] = '\0';
    }
    http_response_buffer_t response = {
        .data = calloc(1, RTI_HTTP_RESPONSE_MAX),
        .capacity = RTI_HTTP_RESPONSE_MAX,
    };
    char url[384];
    const size_t base_length = strlen(CONFIG_MIKU_RTI_DEVICE_API_BASE_URL);
    const int url_length = snprintf(url, sizeof(url), "%s%srtic/v1/realtime/sessions",
                                    CONFIG_MIKU_RTI_DEVICE_API_BASE_URL,
                                    base_length > 0 && CONFIG_MIKU_RTI_DEVICE_API_BASE_URL[base_length - 1] == '/' ? "" : "/");
    if (authorization == NULL || response.data == NULL || url_length < 0 || (size_t)url_length >= sizeof(url)) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    snprintf(authorization, strlen(credentials.api_token) + strlen("Bearer ") + 1,
             "Bearer %s", credentials.api_token);

    const esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &response,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = RTI_HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", authorization);
    esp_http_client_set_post_field(client, request_body, strlen(request_body));
    err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK && status <= 0) {
        ESP_LOGE(TAG, "session request failed: %s", esp_err_to_name(err));
        goto cleanup;
    }
    if (response.overflowed) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    if (status < 200 || status >= 300) {
        log_api_error("session request", status, response.data);
        err = ESP_FAIL;
        goto cleanup;
    }

    err = parse_create_response(response.data, session);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "RTI session created: session_id=%s trace_id=%s", session->session_id, session->trace_id);
        rti_app_set_state(RTI_APP_STATE_READY);
    } else {
        ESP_LOGE(TAG, "invalid create-session response");
        const size_t session_id_length = strlen(session->session_id);
        stop_invalid_session = session_id_length > 0 &&
            strspn(session->session_id,
                   "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == session_id_length;
    }

cleanup:
    if (authorization != NULL) {
        mbedtls_platform_zeroize(authorization, strlen(authorization));
        free(authorization);
    }
    mbedtls_platform_zeroize(&credentials, sizeof(credentials));
    free(response.data);
    free(request_body);
    if (stop_invalid_session) {
        const esp_err_t stop_err = rti_session_stop(session, "invalid_client_endpoint");
        if (stop_err != ESP_OK) {
            ESP_LOGW(TAG, "could not stop session with invalid endpoint: %s",
                     esp_err_to_name(stop_err));
        }
    }
    if (err != ESP_OK) {
        memset(session, 0, sizeof(*session));
        rti_app_set_state(RTI_APP_STATE_ERROR);
    }
    return err;
}

esp_err_t rti_session_stop(const rti_session_t *session, const char *reason)
{
    if (session == NULL || session->session_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    rti_credentials_t credentials;
    esp_err_t err = rti_credentials_load(&credentials);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *request_json = cJSON_CreateObject();
    if (request_json == NULL ||
        !cJSON_AddStringToObject(request_json, "reason", reason == NULL ? "device_stop" : reason)) {
        cJSON_Delete(request_json);
        mbedtls_platform_zeroize(&credentials, sizeof(credentials));
        return ESP_ERR_NO_MEM;
    }
    char *request_body = cJSON_PrintUnformatted(request_json);
    cJSON_Delete(request_json);

    char *authorization = malloc(strlen(credentials.api_token) + strlen("Bearer ") + 1);
    if (authorization != NULL) {
        authorization[0] = '\0';
    }
    http_response_buffer_t response = {
        .data = calloc(1, RTI_HTTP_RESPONSE_MAX),
        .capacity = RTI_HTTP_RESPONSE_MAX,
    };
    char url[512];
    const size_t base_length = strlen(CONFIG_MIKU_RTI_DEVICE_API_BASE_URL);
    const int url_length = snprintf(url, sizeof(url), "%s%srtic/v1/realtime/sessions/%s/stop",
                                    CONFIG_MIKU_RTI_DEVICE_API_BASE_URL,
                                    base_length > 0 && CONFIG_MIKU_RTI_DEVICE_API_BASE_URL[base_length - 1] == '/' ? "" : "/",
                                    session->session_id);
    if (request_body == NULL || authorization == NULL || response.data == NULL ||
        url_length < 0 || (size_t)url_length >= sizeof(url)) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    snprintf(authorization, strlen(credentials.api_token) + strlen("Bearer ") + 1,
             "Bearer %s", credentials.api_token);

    const esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &response,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = RTI_HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", authorization);
    esp_http_client_set_post_field(client, request_body, strlen(request_body));
    err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK && status <= 0) {
        ESP_LOGE(TAG, "session stop failed: %s", esp_err_to_name(err));
        goto cleanup;
    }
    if (response.overflowed) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    if (status < 200 || status >= 300) {
        log_api_error("session stop", status, response.data);
        err = ESP_FAIL;
        goto cleanup;
    }

    ESP_LOGI(TAG, "RTI session stopped: session_id=%s", session->session_id);
    err = ESP_OK;

cleanup:
    if (authorization != NULL) {
        mbedtls_platform_zeroize(authorization, strlen(authorization));
        free(authorization);
    }
    mbedtls_platform_zeroize(&credentials, sizeof(credentials));
    free(response.data);
    free(request_body);
    return err;
}

esp_err_t rti_session_exchange_sdp(const rti_session_t *session, const char *offer_sdp,
                                   char **answer_sdp)
{
    if (session == NULL || offer_sdp == NULL || offer_sdp[0] == '\0' || answer_sdp == NULL ||
        session->rtc_url[0] == '\0' || session->access_token[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    *answer_sdp = NULL;

    cJSON *request_json = cJSON_CreateObject();
    if (request_json == NULL ||
        !cJSON_AddStringToObject(request_json, "type", "offer") ||
        !cJSON_AddStringToObject(request_json, "sdp", offer_sdp)) {
        cJSON_Delete(request_json);
        return ESP_ERR_NO_MEM;
    }
    char *request_body = cJSON_PrintUnformatted(request_json);
    cJSON_Delete(request_json);

    char *authorization = malloc(strlen(session->access_token) + strlen("Bearer ") + 1);
    if (authorization != NULL) {
        authorization[0] = '\0';
    }
    http_response_buffer_t response = {
        .data = calloc(1, RTI_RTC_RESPONSE_MAX),
        .capacity = RTI_RTC_RESPONSE_MAX,
    };
    esp_err_t err = ESP_OK;
    if (request_body == NULL || authorization == NULL || response.data == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    snprintf(authorization, strlen(session->access_token) + strlen("Bearer ") + 1,
             "Bearer %s", session->access_token);

    const esp_http_client_config_t config = {
        .url = session->rtc_url,
        .event_handler = http_event_handler,
        .user_data = &response,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = RTI_HTTP_TIMEOUT_MS,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", authorization);
    esp_http_client_set_post_field(client, request_body, strlen(request_body));
    err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK && status <= 0) {
        ESP_LOGE(TAG, "RTC offer failed: %s", esp_err_to_name(err));
        goto cleanup;
    }
    if (response.overflowed) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    if (status < 200 || status >= 300) {
        log_api_error("RTC offer", status, response.data);
        err = ESP_FAIL;
        goto cleanup;
    }

    cJSON *root = cJSON_Parse(response.data);
    const cJSON *type = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *sdp = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "sdp");
    if (!cJSON_IsString(type) || strcmp(type->valuestring, "answer") != 0 ||
        !cJSON_IsString(sdp) || sdp->valuestring == NULL || sdp->valuestring[0] == '\0') {
        cJSON_Delete(root);
        err = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    *answer_sdp = strdup(sdp->valuestring);
    cJSON_Delete(root);
    if (*answer_sdp == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    ESP_LOGI(TAG, "RTC answer received for session_id=%s", session->session_id);

cleanup:
    if (authorization != NULL) {
        mbedtls_platform_zeroize(authorization, strlen(authorization));
        free(authorization);
    }
    free(response.data);
    free(request_body);
    if (err != ESP_OK) {
        free(*answer_sdp);
        *answer_sdp = NULL;
    }
    return err;
}

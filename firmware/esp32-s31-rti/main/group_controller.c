#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* These transitions run under guard in firmware. The host entry below exercises
 * the same transitions without networking, an RTOS, or a copy of the model. */
typedef struct {
    bool desired;
    bool ready;
    bool pressed;
    bool held;
    bool busy;
    bool link_lost;
    uint32_t generation;
    uint32_t session;
} group_intent_t;

static void intent_cancel(group_intent_t *s)
{
    s->pressed = false;
    s->held = false;
    ++s->generation;
}

static void intent_action(group_intent_t *s, bool stop)
{
    if (stop) {
        intent_cancel(s);
        s->desired = false;
        s->ready = false;
    } else if (!s->desired) {
        intent_cancel(s);
        s->desired = true;
        s->ready = false;
        ++s->session;
    }
}

static void intent_link_lost(group_intent_t *s)
{
    intent_cancel(s);
    s->ready = false;
    s->link_lost = true;
}

static bool intent_begin_connect(group_intent_t *s, uint32_t *ticket)
{
    if (!s->desired) {
        return false;
    }
    intent_cancel(s);
    s->ready = false;
    s->link_lost = false;
    *ticket = s->generation;
    return true;
}

static bool intent_connection_current(const group_intent_t *s, uint32_t ticket)
{
    return s->desired && !s->link_lost && s->generation == ticket;
}

static bool intent_publish_connected(group_intent_t *s, uint32_t ticket)
{
    if (!intent_connection_current(s, ticket)) {
        return false;
    }
    s->ready = true;
    return true;
}

static bool intent_press(group_intent_t *s)
{
    if (!s->ready || !s->desired || s->pressed || s->busy || s->link_lost) {
        return false;
    }
    s->pressed = true;
    s->held = false;
    ++s->generation;
    return true;
}

static bool intent_accept_grant(group_intent_t *s, uint32_t ticket,
                                int64_t deadline, int64_t now,
                                bool renewing, int64_t old_deadline)
{
    if (!s->desired || !s->ready || !s->pressed || s->link_lost ||
        s->generation != ticket || deadline <= now ||
        (renewing && (!s->held || old_deadline <= now))) {
        return false;
    }
    s->held = true;
    return true;
}

static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned year = (unsigned)(y - era * 400);
    const unsigned month = m > 2 ? m - 3 : m + 9;
    const unsigned doy = (153 * month + 2) / 5 + d - 1;
    return (int64_t)era * 146097 + year * 365 + year / 4 - year / 100 + doy - 719468;
}

static int decimal_field(const char *s, size_t count)
{
    int value = 0;
    for (size_t i = 0; i < count; ++i) {
        if (s[i] < '0' || s[i] > '9') {
            return -1;
        }
        value = value * 10 + s[i] - '0';
    }
    return value;
}

static int64_t parse_time(const char *s)
{
    if (!s) {
        return -1;
    }
    const size_t size = strlen(s);
    if (size < 20 || size > 30 || s[4] != '-' || s[7] != '-' ||
        s[10] != 'T' || s[13] != ':' || s[16] != ':') {
        return -1;
    }
    const int y = decimal_field(s, 4);
    const int m = decimal_field(s + 5, 2);
    const int d = decimal_field(s + 8, 2);
    const int h = decimal_field(s + 11, 2);
    const int minute = decimal_field(s + 14, 2);
    const int second = decimal_field(s + 17, 2);
    if (y < 2025 || y > 2100 || m < 1 || m > 12 || d < 1 ||
        h < 0 || h > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
        return -1;
    }
    static const unsigned month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
    if ((unsigned)d > month_days[m - 1] + (unsigned)(m == 2 && leap)) {
        return -1;
    }
    const char *p = s + 19;
    unsigned ms = 0;
    unsigned digits = 0;
    if (*p == '.') {
        ++p;
        while (*p >= '0' && *p <= '9') {
            if (digits < 3) {
                ms = ms * 10 + (unsigned)(*p - '0');
            }
            ++digits;
            ++p;
        }
        if (digits == 0 || digits > 9) {
            return -1;
        }
    }
    while (digits < 3) {
        ms *= 10;
        ++digits;
    }
    if (strcmp(p, "Z") != 0) {
        return -1;
    }
    return ((days_from_civil(y, (unsigned)m, (unsigned)d) * 24 + h) * 3600 +
            minute * 60 + second) * 1000 + ms;
}

static bool format_time(int64_t ms, char out[32])
{
    if (ms < 1735689600000LL || ms >= 4133980800000LL) {
        return false;
    }
    const time_t seconds = (time_t)(ms / 1000);
    struct tm tm;
    if (!gmtime_r(&seconds, &tm) ||
        strftime(out, 20, "%Y-%m-%dT%H:%M:%S", &tm) != 19) {
        return false;
    }
    const unsigned fraction = (unsigned)(ms % 1000);
    return snprintf(out + 19, 13, ".%03uZ", fraction) == 5;
}

#ifdef GROUP_CONTROLLER_HOST_TEST
#include <assert.h>

int main(void)
{
    group_intent_t s = {0};
    uint32_t connection;
    assert(!intent_press(&s));
    intent_action(&s, false);
    const uint32_t first_session = s.session;
    assert(intent_begin_connect(&s, &connection));
    intent_action(&s, true);
    assert(!intent_publish_connected(&s, connection));
    intent_action(&s, false);
    assert(s.session != first_session);
    assert(!intent_publish_connected(&s, connection));
    assert(intent_begin_connect(&s, &connection));
    intent_link_lost(&s);
    assert(!intent_publish_connected(&s, connection));
    assert(intent_begin_connect(&s, &connection));
    assert(intent_publish_connected(&s, connection));
    assert(!s.pressed && !s.held);
    s.busy = true;
    assert(!intent_press(&s));
    s.busy = false;
    assert(intent_press(&s));
    uint32_t request = s.generation;
    intent_cancel(&s);
    assert(!intent_accept_grant(&s, request, 3000, 1000, false, 0));
    assert(!s.pressed && !s.held);
    assert(intent_press(&s));
    const uint32_t new_request = s.generation;
    assert(!intent_accept_grant(&s, request, 3000, 1000, false, 0));
    assert(!intent_accept_grant(&s, new_request, 1000, 1000, false, 0));
    assert(intent_accept_grant(&s, new_request, 3000, 1000, false, 0));
    assert(!intent_accept_grant(&s, new_request, 6000, 3000, true, 3000));
    assert(intent_accept_grant(&s, new_request, 5000, 2000, true, 3000));
    intent_link_lost(&s);
    assert(!intent_accept_grant(&s, new_request, 6000, 3000, true, 5000));
    assert(!s.pressed && !s.held && !s.ready);
    assert(parse_time("2025-01-01T00:00:00Z") == 1735689600000LL);
    assert(parse_time("2028-02-29T23:59:59.123456789Z") > 0);
    const char *invalid[] = {
        "2024-12-31T23:59:59Z", "2025-02-29T00:00:00Z", "2100-02-29T00:00:00Z",
        "2026-04-31T00:00:00Z", "2026-10-24T-1:00:00Z", "2026-10-24T00:-1:00Z",
        "2026-10-24T00:00:-1Z", "2026-10-24T24:00:00Z", "2026-10-24T00:60:00Z",
        "2026-10-24T00:00:60Z", "2026-10-24T00:00:00.Z", "2026-10-24T00:00:00+00:00",
        "2026-10-24T00:00:00.1234567890Z", "2026-10-24T00:00:00Ztrailing"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(parse_time(invalid[i]) == -1);
    }
    char formatted[32];
    assert(format_time(1735689600123LL, formatted));
    assert(strcmp(formatted, "2025-01-01T00:00:00.123Z") == 0);
    assert(parse_time(formatted) == 1735689600123LL);
    assert(!format_time(-1, formatted));
    assert(!format_time(4133980800000LL, formatted));
    puts("group controller host regression: passed (late grant, cancel/repress, expiry, reconnect, RFC3339 bounds)");
    return 0;
}
#else

#include "group_controller.h"
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include "board_display.h"
#include "cJSON.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "group_config.h"
#include "mbedtls/platform_util.h"
#include "webrtc_peer.h"

#define RESPONSE_MAX (48 * 1024)
static const char *TAG = "group_controller";
static portMUX_TYPE guard = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t worker;
static board_audio_t audio;
static group_config_t config;
static group_intent_t intent;
/* Protocol bindings and JSON are owned only by the lifecycle worker. */
static char group_id[81], group_name[129], server_epoch[81], peer_id[81], lease_id[81];
static uint64_t version;
static int64_t server_base_ms, clock_at_us, lease_deadline_us, request_deadline_us;
static int64_t next_renew_us, next_poll_us;
static uint32_t request_generation;
static cJSON *floor_request;
static int last_http_status;
static char ui_detail[160];
static bool ui_active_known, ui_active;
/* One HTTP/TLS handle belongs to the lifecycle worker and one configured
 * identity. Keep it across complete requests, not across STOP/config changes. */
static esp_http_client_handle_t control_client;
static uint64_t http_requests, tls_connections, http_failures;

typedef struct {
    char *data;
    size_t used;
    bool overflow;
} response_t;

static bool string_field(const cJSON *o, const char *key, char *out, size_t cap)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsString(v) || !v->valuestring || strlen(v->valuestring) >= cap) {
        return false;
    }
    strlcpy(out, v->valuestring, cap);
    return true;
}

static int64_t server_now(void)
{
    return server_base_ms + (esp_timer_get_time() - clock_at_us) / 1000;
}

static void random_id(char out[81])
{
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        snprintf(out + i * 2, 3, "%02x", bytes[i]);
    }
}

static bool trusted_clock(void)
{
    const time_t now = time(NULL);
    return now >= 1735689600LL && now < 4133980800LL;
}

static esp_err_t http_event(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_CONNECTED) {
        portENTER_CRITICAL(&guard);
        ++tls_connections;
        portEXIT_CRITICAL(&guard);
    }
    response_t *r = e->user_data;
    if (e->event_id != HTTP_EVENT_ON_DATA || !r || e->data_len <= 0) {
        return ESP_OK;
    }
    if ((size_t)e->data_len >= RESPONSE_MAX - r->used) {
        r->overflow = true;
        return ESP_ERR_NO_MEM;
    }
    memcpy(r->data + r->used, e->data, (size_t)e->data_len);
    r->used += (size_t)e->data_len;
    r->data[r->used] = 0;
    return ESP_OK;
}

static void drop_http_client(void)
{
    if (control_client) {
        /* The payload and response are owned by a single synchronous request.
         * Never leave their addresses in a handle kept for the next transfer. */
        (void)esp_http_client_set_user_data(control_client, NULL);
        (void)esp_http_client_set_post_field(control_client, NULL, 0);
        (void)esp_http_client_cleanup(control_client);
        control_client = NULL;
    }
}

static void record_http_failure(esp_err_t err, bool performed)
{
    portENTER_CRITICAL(&guard);
    ++http_failures;
    portEXIT_CRITICAL(&guard);
    int tls_code = 0, verify_flags = 0;
    esp_err_t tls_status = ESP_OK;
    if (performed && control_client) {
        tls_status = esp_http_client_get_and_clear_last_tls_error(control_client, &tls_code, &verify_flags);
    }
    /* Codes and allocator state only: never log the URL, headers, token, or SDP. */
    ESP_LOGW(TAG, "control HTTP failure: err=%d tls=%d tls_status=%d verify=%d internal=%u largest_internal=%u",
             (int)err, tls_code, (int)tls_status, verify_flags,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

static esp_err_t set_request_body(esp_http_client_handle_t client, const char *payload, int size)
{
    const esp_err_t err = esp_http_client_set_post_field(client, payload, size);
    /* This SDK clears post_data first, then removes Content-Type. An already
     * absent header is a successful idempotent NULL/0 body detach. */
    return payload == NULL && size == 0 && err == ESP_ERR_NOT_FOUND ? ESP_OK : err;
}

static esp_err_t request(const char *path, esp_http_client_method_t method,
                         const cJSON *body, cJSON **result)
{
    portENTER_CRITICAL(&guard);
    ++http_requests;
    portEXIT_CRITICAL(&guard);
    last_http_status = 0;
    if (result) {
        *result = NULL;
    }
    response_t response = {0};
    char *payload = NULL;
    char url[320] = {0}, auth[160] = {0};
    esp_err_t err = ESP_OK;
    bool performed = false;
    bool complete = false;
    bool persistent = false;
    if (!trusted_clock()) {
        err = ESP_ERR_INVALID_STATE;
        goto done;
    }
    if (snprintf(url, sizeof(url), "%s/api/v1%s", config.server_url, path) >= (int)sizeof(url) ||
        snprintf(auth, sizeof(auth), "Bearer %s", config.token) >= (int)sizeof(auth)) {
        err = ESP_ERR_INVALID_ARG;
        goto done;
    }
    response.data = heap_caps_calloc(1, RESPONSE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!response.data) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    payload = body ? cJSON_PrintUnformatted(body) : NULL;
    if (body && !payload) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    const size_t payload_size = payload ? strlen(payload) : 0;
    if (payload_size > RESPONSE_MAX || payload_size > INT_MAX) {
        err = ESP_ERR_INVALID_SIZE;
        goto done;
    }
    const int timeout = method == HTTP_METHOD_POST && strcmp(path, "/media/peers") == 0 ? 8000 : 2500;
    if (!control_client) {
        const esp_http_client_config_t opts = {
            .url = url,
            .cert_pem = config.ca_pem,
            .timeout_ms = timeout,
            .event_handler = http_event,
            .disable_auto_redirect = true,
            .skip_cert_common_name_check = false,
            .buffer_size = 1024
        };
        control_client = esp_http_client_init(&opts);
        if (!control_client) {
            err = ESP_ERR_NO_MEM;
            goto done;
        }
    }
    /* Every request resets all varying fields. The origin/CA identity remains
     * fixed until the worker drops this handle before replacing config. */
    err = esp_http_client_set_user_data(control_client, &response);
    if (err == ESP_OK) {
        err = esp_http_client_set_url(control_client, url);
    }
    if (err == ESP_OK) {
        err = esp_http_client_set_method(control_client, method);
    }
    if (err == ESP_OK) {
        err = esp_http_client_set_timeout_ms(control_client, timeout);
    }
    if (err == ESP_OK) {
        err = esp_http_client_set_header(control_client, "Authorization", auth);
    }
    if (err == ESP_OK) {
        err = esp_http_client_set_header(control_client, "Content-Type", "application/json");
    }
    if (err == ESP_OK) {
        /* Nonempty post_field queries Content-Type before assigning defaults.
         * This SDK reports a missing header as NOT_FOUND, so install JSON
         * headers first. NULL/0 clears the header again for a bodyless GET. */
        err = set_request_body(control_client, payload, (int)payload_size);
    }
    if (err == ESP_OK) {
        performed = true;
        err = esp_http_client_perform(control_client);
        last_http_status = esp_http_client_get_status_code(control_client);
        if (err == ESP_OK) {
            complete = esp_http_client_is_complete_data_received(control_client);
            persistent = esp_http_client_is_persistent_connection(control_client);
        }
    }
    if (response.overflow) {
        err = ESP_ERR_INVALID_SIZE;
    } else if (err == ESP_OK && !complete) {
        err = ESP_ERR_HTTP_INCOMPLETE_DATA;
    } else if (err == ESP_OK && (last_http_status < 200 || last_http_status >= 300)) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK && result) {
        *result = cJSON_ParseWithLengthOpts(response.data, response.used + 1, NULL, true);
        if (!*result) {
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }
done:
    if (control_client) {
        const esp_err_t detach_body = set_request_body(control_client, NULL, 0);
        const esp_err_t detach_response = esp_http_client_set_user_data(control_client, NULL);
        if (err == ESP_OK && (detach_body != ESP_OK || detach_response != ESP_OK)) {
            err = detach_body != ESP_OK ? detach_body : detach_response;
        }
    }
    if (err != ESP_OK) {
        record_http_failure(err, performed);
        if (result) {
            cJSON_Delete(*result);
            *result = NULL;
        }
    }
    /* A failed, partial, oversized, unparsable, or explicitly closed response
     * cannot be reused. No automatic replay of an uncertain mutation occurs. */
    if (err != ESP_OK || !complete || !persistent) {
        drop_http_client();
    }
    mbedtls_platform_zeroize(auth, sizeof(auth));
    if (payload) {
        mbedtls_platform_zeroize(payload, strlen(payload));
        free(payload);
    }
    if (response.data) {
        mbedtls_platform_zeroize(response.data, response.used);
        free(response.data);
    }
    return err;
}

static bool update_ui(const char *detail)
{
    portENTER_CRITICAL(&guard);
    const bool connected = intent.ready;
    const bool speaking = intent.held && intent.pressed;
    const bool active = intent.desired;
    portEXIT_CRITICAL(&guard);
    if (detail) {
        strlcpy(ui_detail, detail, sizeof(ui_detail));
    }
    if (!ui_active_known || ui_active != active) {
        if (board_display_set_group_active(active) != ESP_OK) {
            return false;
        }
        ui_active = active;
        ui_active_known = true;
    }
    return board_display_set_group_state(group_name, ui_detail, connected, speaking) == ESP_OK;
}

/* Lock order is always controller guard -> peer gate lock. Peer gate code never
 * calls back into the controller or display. UI callbacks do no I/O/display. */
static void cancel_locked(void)
{
    webrtc_peer_group_set_sending(false, 0);
    intent_cancel(&intent);
}

static void notify_worker(void)
{
    portENTER_CRITICAL(&guard);
    const TaskHandle_t task = worker;
    portEXIT_CRITICAL(&guard);
    if (task) {
        xTaskNotifyGive(task);
    }
}

static void release_floor(void)
{
    portENTER_CRITICAL(&guard);
    cancel_locked();
    intent.busy = true;
    lease_deadline_us = 0;
    portEXIT_CRITICAL(&guard);
    if (floor_request) {
        (void)request("/floor/release", HTTP_METHOD_POST, floor_request, NULL);
        cJSON_Delete(floor_request);
        floor_request = NULL;
    }
    lease_id[0] = 0;
    request_deadline_us = 0;
    portENTER_CRITICAL(&guard);
    intent.busy = false;
    portEXIT_CRITICAL(&guard);
}

static void disconnect_peer(void)
{
    portENTER_CRITICAL(&guard);
    intent.ready = false;
    portEXIT_CRITICAL(&guard);
    release_floor();
    webrtc_peer_stop();
    if (peer_id[0]) {
        char path[112];
        snprintf(path, sizeof(path), "/media/peers/%s", peer_id);
        (void)request(path, HTTP_METHOD_DELETE, NULL, NULL);
        peer_id[0] = 0;
    }
    drop_http_client();
}

static esp_err_t apply_state(const cJSON *state, int64_t started, int64_t ended)
{
    char id[81], epoch[81], gid[81], time_text[64];
    const cJSON *device = cJSON_GetObjectItemCaseSensitive(state, "device");
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(state, "current_version");
    const cJSON *groups = cJSON_GetObjectItemCaseSensitive(state, "groups");
    if (!string_field(device, "id", id, sizeof(id)) || strcmp(id, config.device_id) ||
        !string_field(state, "server_epoch", epoch, sizeof(epoch)) || !epoch[0] ||
        !string_field(state, "current_group_id", gid, sizeof(gid)) ||
        !string_field(state, "server_time", time_text, sizeof(time_text)) ||
        !cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < 0 ||
        v->valuedouble > 9007199254740991.0 || floor(v->valuedouble) != v->valuedouble ||
        !cJSON_IsArray(groups) || ended < started) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const int64_t stamp = parse_time(time_text);
    if (stamp < 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    char name[sizeof(group_name)] = {0};
    const cJSON *g = NULL;
    bool matched = gid[0] == 0;
    cJSON_ArrayForEach(g, groups) {
        char candidate[81];
        if (string_field(g, "id", candidate, sizeof(candidate)) && strcmp(candidate, gid) == 0) {
            matched = string_field(g, "name", name, sizeof(name)) && name[0];
            break;
        }
    }
    if (!matched) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (peer_id[0] && (strcmp(epoch, server_epoch) || strcmp(gid, group_id) ||
                       (uint64_t)v->valuedouble != version)) {
        disconnect_peer();
    }
    strlcpy(server_epoch, epoch, sizeof(server_epoch));
    strlcpy(group_id, gid, sizeof(group_id));
    strlcpy(group_name, name, sizeof(group_name));
    version = (uint64_t)v->valuedouble;
    /* Add the full request RTT, conservatively shortening local lease life. */
    server_base_ms = stamp + (ended - started) / 1000;
    clock_at_us = ended;
    return ESP_OK;
}

static esp_err_t refresh_state(bool join)
{
    cJSON *out = NULL;
    int64_t started = esp_timer_get_time();
    esp_err_t err = request("/state", HTTP_METHOD_GET, NULL, &out);
    if (err == ESP_OK) {
        err = apply_state(out, started, esp_timer_get_time());
    }
    cJSON_Delete(out);
    if (err != ESP_OK || group_id[0] || !join) {
        return err;
    }
    cJSON *body = cJSON_CreateObject();
    if (!body || !cJSON_AddStringToObject(body, "code", config.group_code)) {
        cJSON_Delete(body);
        return ESP_ERR_NO_MEM;
    }
    out = NULL;
    started = esp_timer_get_time();
    err = request("/memberships", HTTP_METHOD_POST, body, &out);
    cJSON_Delete(body);
    if (err == ESP_OK) {
        err = apply_state(cJSON_GetObjectItemCaseSensitive(out, "state"), started, esp_timer_get_time());
    }
    cJSON_Delete(out);
    return err;
}

static bool connection_current(uint32_t ticket)
{
    portENTER_CRITICAL(&guard);
    const bool current = intent_connection_current(&intent, ticket);
    portEXIT_CRITICAL(&guard);
    return current;
}

static esp_err_t exchange_sdp(const char *offer, char **answer, void *context)
{
    const uint32_t ticket = *(const uint32_t *)context;
    *answer = NULL;
    if (!connection_current(ticket)) {
        return ESP_ERR_INVALID_STATE;
    }
    cJSON *status = NULL;
    esp_err_t err = request("/media", HTTP_METHOD_GET, NULL, &status);
    char replace[81], capability_epoch[81];
    if (err != ESP_OK ||
        !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(status, "available")) ||
        !string_field(status, "server_epoch", capability_epoch, sizeof(capability_epoch)) ||
        strcmp(capability_epoch, server_epoch) ||
        !string_field(status, "peer_id", replace, sizeof(replace))) {
        cJSON_Delete(status);
        return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
    }
    cJSON_Delete(status);
    char deadline[32];
    if (!format_time(server_now() + 8000, deadline)) {
        return ESP_ERR_INVALID_STATE;
    }
    cJSON *body = cJSON_CreateObject();
    cJSON *description = cJSON_CreateObject();
    if (!body || !description ||
        !cJSON_AddStringToObject(body, "peer_id", peer_id) ||
        !cJSON_AddStringToObject(body, "replace_peer_id", replace) ||
        !cJSON_AddStringToObject(body, "group_id", group_id) ||
        !cJSON_AddNumberToObject(body, "current_version", (double)version) ||
        !cJSON_AddStringToObject(body, "server_epoch", server_epoch) ||
        !cJSON_AddStringToObject(body, "valid_until", deadline) ||
        !cJSON_AddStringToObject(description, "type", "offer") ||
        !cJSON_AddStringToObject(description, "sdp", offer)) {
        cJSON_Delete(description);
        cJSON_Delete(body);
        return ESP_ERR_NO_MEM;
    }
    if (!cJSON_AddItemToObject(body, "offer", description)) {
        cJSON_Delete(description);
        cJSON_Delete(body);
        return ESP_ERR_NO_MEM;
    }
    if (!connection_current(ticket)) {
        cJSON_Delete(body);
        return ESP_ERR_INVALID_STATE;
    }
    cJSON *out = NULL;
    err = request("/media/peers", HTTP_METHOD_POST, body, &out);
    cJSON_Delete(body);
    if (err == ESP_OK && !connection_current(ticket)) {
        err = ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK) {
        char answer_peer[81], answer_group[81], answer_epoch[81];
        const cJSON *answer_version = cJSON_GetObjectItemCaseSensitive(out, "current_version");
        const cJSON *desc = cJSON_GetObjectItemCaseSensitive(out, "answer");
        const cJSON *sdp = cJSON_GetObjectItemCaseSensitive(desc, "sdp");
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(desc, "type");
        if (!string_field(out, "peer_id", answer_peer, sizeof(answer_peer)) || strcmp(answer_peer, peer_id) ||
            !string_field(out, "group_id", answer_group, sizeof(answer_group)) || strcmp(answer_group, group_id) ||
            !string_field(out, "server_epoch", answer_epoch, sizeof(answer_epoch)) || strcmp(answer_epoch, server_epoch) ||
            !cJSON_IsNumber(answer_version) || !isfinite(answer_version->valuedouble) ||
            answer_version->valuedouble != (double)version ||
            !cJSON_IsString(type) || !type->valuestring || strcmp(type->valuestring, "answer") ||
            !cJSON_IsString(sdp) || !sdp->valuestring || !sdp->valuestring[0]) {
            err = ESP_ERR_INVALID_RESPONSE;
        } else {
            *answer = strdup(sdp->valuestring);
            if (!*answer) {
                err = ESP_ERR_NO_MEM;
            }
        }
    }
    cJSON_Delete(out);
    return err;
}

static esp_err_t connect_peer(uint32_t configured_session)
{
    uint32_t ticket;
    portENTER_CRITICAL(&guard);
    webrtc_peer_group_set_sending(false, 0);
    const bool begin = intent.session == configured_session &&
                       intent_begin_connect(&intent, &ticket);
    portEXIT_CRITICAL(&guard);
    if (!begin) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = refresh_state(true);
    if (err != ESP_OK || !group_id[0] || !connection_current(ticket)) {
        return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
    }
    random_id(peer_id);
    update_ui("正在连接群音频");
    err = webrtc_peer_start_group(&audio, exchange_sdp, &ticket);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&guard);
        const bool connected = webrtc_peer_group_connected() &&
                               intent_publish_connected(&intent, ticket);
        portEXIT_CRITICAL(&guard);
        if (connected) {
            next_poll_us = esp_timer_get_time() + 1000000;
            update_ui("群音频就绪，按住说话");
            return ESP_OK;
        }
        err = ESP_ERR_INVALID_STATE;
    }
    disconnect_peer();
    return err;
}

static bool accept_floor(const cJSON *out, bool renewing)
{
    char device[81], request_id[81], peer[81], lease[81], expiry[64];
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(out, "floor");
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(floor_request, "request_id");
    if (!cJSON_IsString(r) || !r->valuestring ||
        !string_field(f, "device_id", device, sizeof(device)) || strcmp(device, config.device_id) ||
        !string_field(f, "request_id", request_id, sizeof(request_id)) || strcmp(request_id, r->valuestring) ||
        !string_field(f, "peer_id", peer, sizeof(peer)) || strcmp(peer, peer_id) ||
        !string_field(f, "lease_id", lease, sizeof(lease)) || !lease[0] ||
        (renewing && strcmp(lease, lease_id)) ||
        !string_field(f, "expires_at", expiry, sizeof(expiry))) {
        return false;
    }
    const int64_t expiry_ms = parse_time(expiry);
    const int64_t remaining = expiry_ms - server_now() - 100;
    if (expiry_ms < 0 || remaining <= 0 || remaining > 3000) {
        return false;
    }
    const int64_t now = esp_timer_get_time();
    const int64_t deadline = now + remaining * 1000;
    portENTER_CRITICAL(&guard);
    const bool accepted = (renewing || now < request_deadline_us) &&
                          webrtc_peer_group_connected() &&
                          intent_accept_grant(&intent, request_generation, deadline, now,
                                              renewing, lease_deadline_us);
    if (accepted) {
        strlcpy(lease_id, lease, sizeof(lease_id));
        lease_deadline_us = deadline;
        /* Linearized with UI release/stop: neither can run between validating
         * this generation and opening the gate. No display/HTTP under guard. */
        webrtc_peer_group_set_sending(true, deadline);
    }
    portEXIT_CRITICAL(&guard);
    if (accepted) {
        next_renew_us = now + 1000000;
    }
    return accepted;
}

static cJSON *new_floor_request(void)
{
    char id[81];
    random_id(id);
    cJSON *body = cJSON_CreateObject();
    if (!body || !cJSON_AddStringToObject(body, "group_id", group_id) ||
        !cJSON_AddNumberToObject(body, "current_version", (double)version) ||
        !cJSON_AddStringToObject(body, "server_epoch", server_epoch) ||
        !cJSON_AddStringToObject(body, "peer_id", peer_id) ||
        !cJSON_AddStringToObject(body, "request_id", id)) {
        cJSON_Delete(body);
        return NULL;
    }
    return body;
}

static void acquire_floor(void)
{
    portENTER_CRITICAL(&guard);
    if (!intent.desired || !intent.ready || !intent.pressed || intent.busy || intent.link_lost) {
        portEXIT_CRITICAL(&guard);
        return;
    }
    request_generation = intent.generation;
    intent.busy = true;
    portEXIT_CRITICAL(&guard);
    floor_request = new_floor_request();
    char until[32];
    cJSON *body = floor_request ? cJSON_Duplicate(floor_request, true) : NULL;
    request_deadline_us = esp_timer_get_time() + 5000000;
    if (!body || !format_time(server_now() + 5000, until) ||
        !cJSON_AddStringToObject(body, "valid_until", until)) {
        cJSON_Delete(body);
        release_floor();
        update_ui("麦权申请失败，请松手后重试");
        return;
    }
    cJSON *out = NULL;
    const esp_err_t err = request("/floor/acquire", HTTP_METHOD_POST, body, &out);
    cJSON_Delete(body);
    if (err != ESP_OK || !accept_floor(out, false)) {
        cJSON_Delete(out);
        /* Even an unknown/late grant is tombstoned using the original request. */
        release_floor();
        update_ui("未获麦权，请松手后重试");
        return;
    }
    cJSON_Delete(out);
    portENTER_CRITICAL(&guard);
    intent.busy = false;
    portEXIT_CRITICAL(&guard);
    update_ui("正在发送，松手停发");
}

static void renew_floor(void)
{
    portENTER_CRITICAL(&guard);
    const bool current = intent.desired && intent.ready && intent.pressed && intent.held &&
                         !intent.link_lost && intent.generation == request_generation &&
                         esp_timer_get_time() < lease_deadline_us;
    if (!current) {
        portEXIT_CRITICAL(&guard);
        release_floor();
        update_ui("麦权已结束，请重新按住");
        return;
    }
    intent.busy = true;
    portEXIT_CRITICAL(&guard);
    cJSON *body = floor_request ? cJSON_Duplicate(floor_request, true) : NULL;
    if (!body || !cJSON_AddStringToObject(body, "lease_id", lease_id)) {
        cJSON_Delete(body);
        release_floor();
        update_ui("麦权已结束，请重新按住");
        return;
    }
    cJSON *out = NULL;
    const esp_err_t err = request("/floor/renew", HTTP_METHOD_POST, body, &out);
    cJSON_Delete(body);
    if (err != ESP_OK || !accept_floor(out, true)) {
        cJSON_Delete(out);
        release_floor();
        update_ui("麦权已结束，请重新按住");
        return;
    }
    cJSON_Delete(out);
    portENTER_CRITICAL(&guard);
    intent.busy = false;
    portEXIT_CRITICAL(&guard);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t event, void *data)
{
    (void)arg;
    (void)base;
    (void)event;
    (void)data;
    portENTER_CRITICAL(&guard);
    webrtc_peer_group_set_sending(false, 0);
    intent_link_lost(&intent);
    portEXIT_CRITICAL(&guard);
    notify_worker();
}

static void run(void *arg)
{
    (void)arg;
    int64_t retry_at = 0;
    bool configured = false;
    bool stopped_published = false;
    uint32_t configured_session = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        portENTER_CRITICAL(&guard);
        const group_intent_t snapshot = intent;
        const int64_t deadline = lease_deadline_us;
        portEXIT_CRITICAL(&guard);
        if (!snapshot.desired) {
            if (configured || peer_id[0] || floor_request) {
                disconnect_peer();
                mbedtls_platform_zeroize(&config, sizeof(config));
                configured = false;
                group_id[0] = 0;
                group_name[0] = 0;
            }
            if (!stopped_published) {
                stopped_published = update_ui("已停止，点击连接群音频");
            }
            retry_at = 0;
            continue;
        }
        stopped_published = false;
        if (!configured || configured_session != snapshot.session) {
            /* A rapid STOP/START can arrive while HTTP is pending. The session
             * number preserves that boundary even if the worker never saw false.
             * Retire the old peer/request with OLD credentials before loading NEW. */
            if (configured || peer_id[0] || floor_request) {
                disconnect_peer();
                mbedtls_platform_zeroize(&config, sizeof(config));
                configured = false;
            }
            if (esp_timer_get_time() < retry_at) {
                continue;
            }
            if (group_config_load(&config) != ESP_OK) {
                update_ui("请通过 USB 配置群服务");
                retry_at = esp_timer_get_time() + 3000000;
                continue;
            }
            configured = true;
            configured_session = snapshot.session;
            group_id[0] = 0;
            group_name[0] = 0;
            server_epoch[0] = 0;
            version = 0;
        }
        if (!trusted_clock()) {
            if (peer_id[0] || floor_request) {
                disconnect_peer();
            }
            drop_http_client();
            update_ui("请通过 USB 同步当前时间");
            continue;
        }
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
            if (peer_id[0] || floor_request) {
                disconnect_peer();
            }
            drop_http_client();
            update_ui("等待 Wi-Fi 恢复");
            continue;
        }
        if (snapshot.link_lost && peer_id[0]) {
            disconnect_peer();
            retry_at = esp_timer_get_time() + 1000000;
            continue;
        }
        if (!snapshot.ready) {
            if (esp_timer_get_time() < retry_at) {
                continue;
            }
            update_ui("正在连接群服务");
            const esp_err_t err = connect_peer(configured_session);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "group connect failed: %s HTTP=%d", esp_err_to_name(err), last_http_status);
                update_ui("群连接失败，请检查服务或重试");
                retry_at = esp_timer_get_time() + 3000000;
            }
            continue;
        }
        if (!webrtc_peer_group_connected()) {
            disconnect_peer();
            retry_at = esp_timer_get_time() + 1000000;
            update_ui("群音频连接中断，已停发");
            continue;
        }
        if (floor_request && (!snapshot.pressed || snapshot.generation != request_generation ||
                              (snapshot.held && esp_timer_get_time() >= deadline))) {
            release_floor();
            update_ui("群音频就绪，按住说话");
            continue;
        }
        if (snapshot.pressed && !floor_request) {
            acquire_floor();
            continue;
        }
        if (snapshot.held && esp_timer_get_time() >= next_renew_us) {
            renew_floor();
            continue;
        }
        if (esp_timer_get_time() >= next_poll_us) {
            const esp_err_t err = refresh_state(false);
            next_poll_us = esp_timer_get_time() + 1000000;
            if (err != ESP_OK) {
                disconnect_peer();
                retry_at = esp_timer_get_time() + 1000000;
                update_ui("控制连接中断，已停发");
            }
        }
    }
}

void group_controller_ptt(bool down, void *context)
{
    (void)context;
    portENTER_CRITICAL(&guard);
    if (!down) {
        cancel_locked();
    } else {
        (void)intent_press(&intent);
    }
    portEXIT_CRITICAL(&guard);
    notify_worker();
}

void group_controller_action(bool stop, void *context)
{
    (void)context;
    portENTER_CRITICAL(&guard);
    if (stop) {
        webrtc_peer_group_set_sending(false, 0);
    }
    intent_action(&intent, stop);
    portEXIT_CRITICAL(&guard);
    notify_worker();
}

static void volume(uint8_t value, bool persist, void *context)
{
    (void)context;
    (void)board_audio_set_output_volume(&audio, value, persist);
}

esp_err_t group_controller_init(const board_audio_t *board)
{
    if (!board || !board->microphone || !board->speaker || worker) {
        return ESP_ERR_INVALID_STATE;
    }
    audio = *board;
    (void)board_audio_set_output_volume(&audio, 0, false);
    esp_err_t err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = board_display_show_group(group_controller_action, group_controller_ptt, volume, 0, NULL);
    if (err == ESP_OK) {
        update_ui(group_config_ready() ? "已配置，点击连接群音频" : "请通过 USB 配置群服务");
        TaskHandle_t task = NULL;
        if (xTaskCreate(run, "group_lifecycle", 20 * 1024, NULL, 10, &task) == pdPASS) {
            portENTER_CRITICAL(&guard);
            worker = task;
            portEXIT_CRITICAL(&guard);
            return ESP_OK;
        }
        err = ESP_ERR_NO_MEM;
    }
    (void)esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event);
    return err;
}

void group_controller_print_status(void)
{
    portENTER_CRITICAL(&guard);
    const group_intent_t snapshot = intent;
    const int64_t deadline = lease_deadline_us;
    const uint64_t requests = http_requests;
    const uint64_t connections = tls_connections;
    const uint64_t failures = http_failures;
    portEXIT_CRITICAL(&guard);
    const bool connected = snapshot.ready && webrtc_peer_group_connected();
    const bool holding = connected && snapshot.held && snapshot.pressed && esp_timer_get_time() < deadline;
    webrtc_group_stats_t stats = {0};
    webrtc_peer_group_get_stats(&stats);
    printf("INTERCOM_STATUS {\"configured\":%s,\"desired\":%s,\"connected\":%s,\"pressed\":%s,\"held\":%s,\"busy\":%s,"
           "\"source_frames\":%" PRIu64 ",\"sent_packets\":%" PRIu64 ",\"received_packets\":%" PRIu64 ",\"decoded_samples\":%" PRIu64 ","
           "\"source_rms_peak\":%.6f,\"remote_rms_peak\":%.6f,\"heap_internal_free\":%u,"
           "\"http_requests\":%" PRIu64 ",\"tls_connections\":%" PRIu64 ",\"http_failures\":%" PRIu64 "}\n",
           group_config_ready() ? "true" : "false", snapshot.desired ? "true" : "false", connected ? "true" : "false",
           snapshot.pressed ? "true" : "false", holding ? "true" : "false", snapshot.busy ? "true" : "false",
           stats.source_frames, stats.sent_packets, stats.received_packets, stats.decoded_samples,
           (double)stats.source_rms_peak, (double)stats.remote_rms_peak,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), requests, connections, failures);
    fflush(stdout);
}
#endif

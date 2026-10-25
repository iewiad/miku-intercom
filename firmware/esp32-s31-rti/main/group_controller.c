#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "group_switch_journal.h"
#include "group_voice_diagnostics.h"

/* These transitions run under guard in firmware. The host entry below exercises
 * the same transitions without networking, an RTOS, or a copy of the model. */
typedef struct {
    bool desired;
    bool ready;
    bool pressed;
    bool held;
    bool busy;
    bool link_lost;
    bool switch_pending;
    bool command_busy;
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
    if (!s->desired || s->switch_pending || s->command_busy) {
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
    return s->desired && !s->link_lost && !s->switch_pending && !s->command_busy && s->generation == ticket;
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
    if (!s->ready || !s->desired || s->pressed || s->busy || s->link_lost || s->switch_pending || s->command_busy) {
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
    if (!s->desired || !s->ready || !s->pressed || s->link_lost || s->switch_pending || s->command_busy ||
        s->generation != ticket || deadline <= now ||
        (renewing && (!s->held || old_deadline <= now))) {
        return false;
    }
    s->held = true;
    return true;
}

static bool intent_queue_switch(group_intent_t *s, bool known,
                                 uint32_t current_view, uint32_t selected_view)
{
    if (!s->desired || !known || s->link_lost || s->switch_pending || s->command_busy ||
        current_view != selected_view) {
        return false;
    }
    intent_cancel(s);
    s->ready = false;
    s->switch_pending = true;
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
    intent_action(&s, true);
    intent_action(&s, false);
    s.ready = true;
    s.link_lost = false;
    assert(!intent_queue_switch(&s, true, 4, 3));
    assert(!s.switch_pending);
    assert(intent_queue_switch(&s, true, 4, 4));
    assert(!s.ready && !s.pressed && !s.held && s.switch_pending);
    assert(!intent_press(&s));
    assert(!intent_begin_connect(&s, &connection));
    intent_action(&s, true);
    intent_action(&s, false);
    intent_link_lost(&s);
    assert(s.switch_pending);
    assert(!intent_begin_connect(&s, &connection));
    group_switch_journal_t journal = {
        .format = GROUP_SWITCH_JOURNAL_FORMAT,
        .server_url = "https://192.168.1.2:1234", .device_id = "device_fixture",
        .operation_id = "operation_fixture", .group_id = "target_fixture", .expected_version = 4
    };
    assert(group_switch_journal_valid(&journal));
    assert(group_switch_journal_bound(&journal, journal.server_url, journal.device_id));
    assert(!group_switch_journal_bound(&journal, "https://192.168.1.3:1234", journal.device_id));
    assert(!group_switch_journal_bound(&journal, journal.server_url, "other_identity"));
    group_switch_terminal_t terminal = {
        .operation_id = "operation_fixture", .group_id = "target_fixture", .expected_version = 4,
        .status = "applied", .current_group_id = "target_fixture", .current_version = 5
    };
    assert(group_switch_terminal_matches(&journal, &terminal));
    assert(group_switch_state_confirms(&terminal, "target_fixture", 5));
    assert(!group_switch_state_confirms(&terminal, "target_fixture", 4));
    assert(!group_switch_state_confirms(&terminal, "different_group", 5));
    assert(group_switch_state_confirms(&terminal, "different_group", 6));
    terminal.expected_version = 5;
    assert(!group_switch_terminal_matches(&journal, &terminal));
    terminal.expected_version = 4;
    strcpy(terminal.status, "pending");
    assert(!group_switch_terminal_matches(&journal, &terminal));
    strcpy(terminal.status, "rejected");
    assert(!group_switch_terminal_matches(&journal, &terminal));
    strcpy(terminal.error_code, "version_conflict");
    assert(group_switch_terminal_matches(&journal, &terminal));
    journal.operation_id[0] = '/';
    assert(!group_switch_journal_valid(&journal));
    group_intent_t cmd = {.desired=true, .ready=true, .command_busy=true};
    assert(!intent_press(&cmd));
    assert(!intent_begin_connect(&cmd, &connection));
    assert(!intent_queue_switch(&cmd, true, 1, 1));
    assert(!intent_accept_grant(&cmd, cmd.generation, 200, 100, false, 0));
    cmd.command_busy = false;
    assert(intent_begin_connect(&cmd, &connection));
    assert(intent_publish_connected(&cmd, connection));
    puts("group controller host regression: passed (late grant, expiry, switch fencing, journal binding, terminal replay/state, RFC3339 bounds)");
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
#include "freertos/semphr.h"
#include "group_config.h"
#include "group_command_state.h"
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
static group_voice_api_error_t last_voice_api_error;
/* Compatibility auto-join is worker-owned and tried once per loaded session. */
static bool legacy_join_attempted;
static char ui_detail[160];
static bool ui_active_known, ui_active;
/* One HTTP/TLS handle belongs to the lifecycle worker and one configured
 * identity. Keep it across complete requests, not across STOP/config changes. */
static esp_http_client_handle_t control_client;
static uint64_t http_requests, tls_connections, http_failures;
#define GROUP_CHOICES_MAX 128
static SemaphoreHandle_t catalog_lock;
static board_group_item_t *catalog_items;
static size_t catalog_count;
static bool catalog_known;
static uint32_t view_generation = 1;
static uint64_t published_version;
static char published_group_id[81];
static size_t published_group_count;
static bool selection_queued;
static char selected_group_id[81];
static uint64_t selected_expected_version;
static group_switch_journal_t pending_operation;
static bool pending_saved, journal_blocked;
typedef enum { SWITCH_IDLE, SWITCH_QUEUED, SWITCH_CONFIRMING, SWITCH_MISMATCH, SWITCH_JOURNAL_ERROR } switch_phase_t;
static switch_phase_t switch_phase;
static char switch_operation_id[81], switch_target_id[81];
static uint32_t choices_view;
static bool choices_published, choices_known, choices_selectable, choices_switching;
static group_command_state_t command;
/* RTA objects belong only to this lifecycle worker, never an event callback. */
static rti_session_t command_session;
static bool command_session_live, command_peer_live;
static int64_t command_deadline_us, command_cleanup_retry_us;
static char command_target[81], command_result[160];
static uint64_t command_expected_version;


static void advance_view_locked(void)
{
    if (++view_generation == 0) {
        ++view_generation;
    }
}

typedef struct {
    char *data;
    size_t used;
    bool overflow;
    bool observed;
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
    if (r && (e->event_id == HTTP_EVENT_ON_HEADER || e->event_id == HTTP_EVENT_ON_DATA ||
              e->event_id == HTTP_EVENT_ON_FINISH)) {
        r->observed = true;
    }
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

static bool voice_error_envelope_valid(const char *data, size_t used)
{
    bool string = false;
    unsigned depth = 0;
    unsigned structural_tokens = 0;
    for (size_t i = 0; i < used; ++i) {
        const unsigned char c = (unsigned char)data[i];
        if (c >= 0x80) {
            unsigned remaining;
            uint32_t value, minimum;
            if (c >= 0xc2 && c <= 0xdf) { remaining = 1; value = c & 0x1f; minimum = 0x80; }
            else if (c >= 0xe0 && c <= 0xef) { remaining = 2; value = c & 0x0f; minimum = 0x800; }
            else if (c >= 0xf0 && c <= 0xf4) { remaining = 3; value = c & 0x07; minimum = 0x10000; }
            else return false;
            if (remaining > used - i - 1) return false;
            while (remaining--) {
                const unsigned char next = (unsigned char)data[++i];
                if ((next & 0xc0) != 0x80) return false;
                value = (value << 6) | (next & 0x3f);
            }
            if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
            continue;
        }
        if (string) {
            if (c < 0x20) return false;
            if (c == '"') string = false;
            else if (c == '\\') {
                if (++i >= used) return false;
                const char escape = data[i];
                if ((unsigned char)escape < 0x20) return false;
                if (escape == 'u') {
                    if (used - i - 1 < 4) return false;
                    unsigned value = 0;
                    for (unsigned n = 0; n < 4; ++n) {
                        const char digit = data[++i];
                        value <<= 4;
                        if (digit >= '0' && digit <= '9') value |= (unsigned)(digit - '0');
                        else if (digit >= 'a' && digit <= 'f') value |= (unsigned)(digit - 'a' + 10);
                        else if (digit >= 'A' && digit <= 'F') value |= (unsigned)(digit - 'A' + 10);
                        else return false;
                    }
                    if (!value) return false; /* cJSON strings are NUL-terminated. */
                } else if (!strchr("\"\\/bfnrt", escape)) return false;
            }
        } else {
            if ((c == '{' || c == '[' || c == ',' || c == ':') && ++structural_tokens > 64) return false;
            if (c == '"') string = true;
            else if (c == '{' || c == '[') { if (++depth > 8) return false; }
            else if (c == '}' || c == ']') { if (!depth) return false; --depth; }
            else if (c < 0x20 && c != '\r' && c != '\n' && c != '\t') return false;
        }
    }
    return !string && depth == 0;
}

static group_voice_api_error_t read_voice_api_error(const char *data, size_t used)
{
    /* Bound bytes, depth and structure before cJSON's allocation/recursion.
     * Error/code keys must be unique after unescaping; decoded NUL is rejected.
     * None of this secondary envelope or its message is retained or logged. */
    if (!data || !used || used > 2048 || !voice_error_envelope_valid(data, used)) return GROUP_VOICE_API_OTHER;
    cJSON *json = cJSON_ParseWithLengthOpts(data, used + 1, NULL, true);
    unsigned errors = 0, codes = 0;
    const cJSON *child;
    cJSON_ArrayForEach(child, json) {
        if (child->string && !strcmp(child->string, "error")) ++errors;
    }
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(json, "error");
    cJSON_ArrayForEach(child, error) {
        if (child->string && !strcmp(child->string, "code")) ++codes;
    }
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(error, "code");
    const group_voice_api_error_t result = group_voice_api_error_code(
        cJSON_IsObject(json) && cJSON_IsObject(error) && errors == 1 && codes == 1 &&
        cJSON_IsString(code) ? code->valuestring : NULL);
    cJSON_Delete(json);
    return result;
}

static esp_err_t request(const char *path, esp_http_client_method_t method,
                         const cJSON *body, cJSON **result)
{
    portENTER_CRITICAL(&guard);
    ++http_requests;
    portEXIT_CRITICAL(&guard);
    last_http_status = 0;
    last_voice_api_error = GROUP_VOICE_API_NONE;
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
        /* IDF can retain the previous response status when connect/send fails
         * before current response headers arrive on a reused handle. */
        const int status = err == ESP_OK || response.observed ? esp_http_client_get_status_code(control_client) : 0;
        last_http_status = status > 0 ? status : 0;
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
    if (err != ESP_OK && complete && !response.overflow &&
        (last_http_status < 200 || last_http_status >= 300) &&
        strcmp(path, "/voice/resolve") == 0) {
        last_voice_api_error = read_voice_api_error(response.data, response.used);
    }
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
    const bool connected = intent.ready && catalog_known && !intent.switch_pending;
    const bool speaking = intent.held && intent.pressed;
    const bool active = intent.desired;
    const bool known = catalog_known;
    const bool switching = intent.switch_pending;
    const bool selectable = active && known && !switching && !intent.link_lost && command.kind == GROUP_COMMAND_NONE;
    const uint32_t command_revision = command.revision;
    const bool joining = command.kind == GROUP_COMMAND_JOIN;
    const bool voice = command.kind == GROUP_COMMAND_VOICE;
    const bool can_cancel = voice && !command.cancelled && !command.submitted && command.phase != GROUP_COMMAND_RECOVERING;
    const uint32_t view = view_generation;
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
    if (!choices_published || choices_view != view || choices_known != known ||
        choices_selectable != selectable || choices_switching != switching) {
        if (board_display_set_group_choices(catalog_items, catalog_count, group_id, view,
                                             known, selectable, switching) != ESP_OK) {
            return false;
        }
        choices_view = view;
        choices_known = known;
        choices_selectable = selectable;
        choices_switching = switching;
        choices_published = true;
    }
    if (board_display_set_group_command_state(command_revision, joining, voice, can_cancel, ui_detail) != ESP_OK) return false;
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
    webrtc_peer_group_suspend_media();
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

static esp_err_t reject_state(esp_err_t err)
{
    portENTER_CRITICAL(&guard);
    webrtc_peer_group_suspend_media();
    intent_cancel(&intent);
    intent.ready = false;
    if (catalog_known) {
        catalog_known = false;
        advance_view_locked();
    }
    portEXIT_CRITICAL(&guard);
    return err;
}

static void forget_catalog(void)
{
    xSemaphoreTake(catalog_lock, portMAX_DELAY);
    board_group_item_t *old = catalog_items;
    catalog_items = NULL;
    catalog_count = 0;
    portENTER_CRITICAL(&guard);
    const bool changed = catalog_known || published_group_count || published_version || published_group_id[0];
    catalog_known = false;
    published_group_count = 0;
    published_version = 0;
    published_group_id[0] = 0;
    if (changed) {
        advance_view_locked();
    }
    portEXIT_CRITICAL(&guard);
    xSemaphoreGive(catalog_lock);
    free(old);
    group_id[0] = 0;
    group_name[0] = 0;
    server_epoch[0] = 0;
    version = 0;
}

static bool json_version(const cJSON *object, const char *key, uint64_t *value)
{
    const cJSON *number = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(number) || !isfinite(number->valuedouble) || number->valuedouble < 0 ||
        number->valuedouble > (double)GROUP_SWITCH_MAX_VERSION || floor(number->valuedouble) != number->valuedouble) {
        return false;
    }
    *value = (uint64_t)number->valuedouble;
    return true;
}

static esp_err_t apply_state(const cJSON *state, int64_t started, int64_t ended)
{
    char id[81], epoch[81], gid[81], time_text[64];
    uint64_t next_version;
    const cJSON *device = cJSON_GetObjectItemCaseSensitive(state, "device");
    const cJSON *groups = cJSON_GetObjectItemCaseSensitive(state, "groups");
    if (!string_field(device, "id", id, sizeof(id)) || strcmp(id, config.device_id) ||
        !string_field(state, "server_epoch", epoch, sizeof(epoch)) || !epoch[0] ||
        !string_field(state, "current_group_id", gid, sizeof(gid)) ||
        (gid[0] && !group_switch_identifier(gid)) ||
        !string_field(state, "server_time", time_text, sizeof(time_text)) ||
        !json_version(state, "current_version", &next_version) || next_version < version ||
        !cJSON_IsArray(groups) || ended < started) {
        return reject_state(ESP_ERR_INVALID_RESPONSE);
    }
    const int64_t stamp = parse_time(time_text);
    const int count = cJSON_GetArraySize(groups);
    if (stamp < 0 || count < 0 || count > GROUP_CHOICES_MAX) {
        return reject_state(ESP_ERR_INVALID_RESPONSE);
    }
    board_group_item_t *items = count ? heap_caps_calloc((size_t)count, sizeof(*items), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (count && !items) {
        return reject_state(ESP_ERR_NO_MEM);
    }
    char name[sizeof(group_name)] = {0};
    bool matched = gid[0] == 0;
    const cJSON *g = NULL;
    int index = 0;
    cJSON_ArrayForEach(g, groups) {
        if (!string_field(g, "id", items[index].id, sizeof(items[index].id)) ||
            !group_switch_identifier(items[index].id) ||
            !string_field(g, "name", items[index].name, sizeof(items[index].name)) || !items[index].name[0]) {
            free(items);
            return reject_state(ESP_ERR_INVALID_RESPONSE);
        }
        for (int previous = 0; previous < index; ++previous) {
            if (!strcmp(items[previous].id, items[index].id) || !strcmp(items[previous].name, items[index].name)) {
                free(items);
                return reject_state(ESP_ERR_INVALID_RESPONSE);
            }
        }
        if (!strcmp(items[index].id, gid)) {
            matched = true;
            strlcpy(name, items[index].name, sizeof(name));
        }
        ++index;
    }
    if (!matched) {
        free(items);
        return reject_state(ESP_ERR_INVALID_RESPONSE);
    }
    const bool binding_changed = strcmp(epoch, server_epoch) || strcmp(gid, group_id) || next_version != version;
    if (peer_id[0] && binding_changed) {
        disconnect_peer();
    }
    xSemaphoreTake(catalog_lock, portMAX_DELAY);
    board_group_item_t *old = catalog_items;
    const bool changed = binding_changed || (size_t)count != catalog_count ||
                         (count && memcmp(old, items, (size_t)count * sizeof(*items)) != 0);
    catalog_items = items;
    catalog_count = (size_t)count;
    strlcpy(server_epoch, epoch, sizeof(server_epoch));
    strlcpy(group_id, gid, sizeof(group_id));
    strlcpy(group_name, name, sizeof(group_name));
    version = next_version;
    portENTER_CRITICAL(&guard);
    if (changed || !catalog_known) {
        advance_view_locked();
    }
    catalog_known = true;
    published_version = version;
    published_group_count = catalog_count;
    strlcpy(published_group_id, group_id, sizeof(published_group_id));
    portEXIT_CRITICAL(&guard);
    xSemaphoreGive(catalog_lock);
    free(old);
    server_base_ms = stamp + (ended - started) / 1000;
    clock_at_us = ended;
    (void)update_ui(NULL);
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
    if (err != ESP_OK) {
        return reject_state(err);
    }
    if (group_id[0] || !join || !config.group_code[0] || legacy_join_attempted) {
        return ESP_OK;
    }
    /* A stale saved code must not repeatedly consume the identity's bad-code
     * allowance and prevent a correct code entered through the keypad. */
    legacy_join_attempted = true;
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
    if (err == ESP_OK && !group_id[0] && connection_current(ticket)) {
        (void)update_ui("尚未加入群组，点击加入群组输入口令");
        return ESP_ERR_NOT_FOUND;
    }
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
    webrtc_peer_group_suspend_media();
    intent_link_lost(&intent);
    if (command.kind == GROUP_COMMAND_VOICE) {
        (void)group_command_cancel(&command);
        webrtc_peer_command_cancel();
    }
    catalog_known = false;
    advance_view_locked();
    portEXIT_CRITICAL(&guard);
    notify_worker();
}

static void reload_switch_journal(void)
{
    group_switch_journal_t record;
    const esp_err_t err = group_switch_journal_load(&record);
    pending_saved = err == ESP_OK;
    journal_blocked = err != ESP_OK && err != ESP_ERR_NOT_FOUND;
    if (pending_saved) {
        pending_operation = record;
    } else {
        memset(&pending_operation, 0, sizeof(pending_operation));
    }
    portENTER_CRITICAL(&guard);
    intent.switch_pending = pending_saved || journal_blocked || selection_queued;
    switch_phase = journal_blocked ? SWITCH_JOURNAL_ERROR : pending_saved ? SWITCH_CONFIRMING : SWITCH_IDLE;
    strlcpy(switch_operation_id, pending_saved ? record.operation_id : "", sizeof(switch_operation_id));
    strlcpy(switch_target_id, pending_saved ? record.group_id : "", sizeof(switch_target_id));
    advance_view_locked();
    portEXIT_CRITICAL(&guard);
}

static void finish_unsent_switch(const char *detail)
{
    portENTER_CRITICAL(&guard);
    intent_cancel(&intent);
    intent.ready = false;
    selection_queued = false;
    intent.switch_pending = pending_saved || journal_blocked;
    switch_phase = journal_blocked ? SWITCH_JOURNAL_ERROR : pending_saved ? SWITCH_CONFIRMING : SWITCH_IDLE;
    if (!intent.switch_pending) {
        switch_operation_id[0] = 0;
        switch_target_id[0] = 0;
    }
    advance_view_locked();
    portEXIT_CRITICAL(&guard);
    (void)update_ui(detail);
}

static void prepare_selected_switch(void)
{
    char target[81];
    uint64_t expected;
    portENTER_CRITICAL(&guard);
    if (!selection_queued) {
        portEXIT_CRITICAL(&guard);
        return;
    }
    strlcpy(target, selected_group_id, sizeof(target));
    expected = selected_expected_version;
    selection_queued = false;
    portEXIT_CRITICAL(&guard);
    disconnect_peer();
    bool member = false;
    for (size_t i = 0; i < catalog_count; ++i) {
        if (!strcmp(catalog_items[i].id, target)) {
            member = true;
            break;
        }
    }
    if (!member) {
        finish_unsent_switch("目标不在已加入群列表中，请刷新后重试");
        return;
    }
    if (!strcmp(target, group_id) && expected == version) {
        finish_unsent_switch("当前已在此群，正在恢复群音频");
        return;
    }
    group_switch_journal_t record = {.format = GROUP_SWITCH_JOURNAL_FORMAT, .expected_version = expected};
    strlcpy(record.server_url, config.server_url, sizeof(record.server_url));
    strlcpy(record.device_id, config.device_id, sizeof(record.device_id));
    strlcpy(record.group_id, target, sizeof(record.group_id));
    random_id(record.operation_id);
    const esp_err_t err = group_switch_journal_save(&record);
    if (err != ESP_OK) {
        /* Save/commit can fail ambiguously. Read the durable slot rather than
         * overwriting an older operation or guessing that no journal exists. */
        reload_switch_journal();
        finish_unsent_switch((pending_saved || journal_blocked) ? "切群记录尚未确认，请稍后重试" : "未提交切群，保存失败，请重试");
        return;
    }
    pending_operation = record;
    pending_saved = true;
    journal_blocked = false;
    portENTER_CRITICAL(&guard);
    intent.switch_pending = true;
    switch_phase = SWITCH_CONFIRMING;
    strlcpy(switch_operation_id, record.operation_id, sizeof(switch_operation_id));
    strlcpy(switch_target_id, record.group_id, sizeof(switch_target_id));
    portEXIT_CRITICAL(&guard);
    (void)update_ui("正在确认切群结果，确认前暂停音频");
}

static bool switch_session_active(uint32_t session)
{
    portENTER_CRITICAL(&guard);
    const bool active = intent.desired && intent.session == session;
    portEXIT_CRITICAL(&guard);
    return active;
}

static bool parse_switch_terminal(const cJSON *response, group_switch_terminal_t *terminal)
{
    memset(terminal, 0, sizeof(*terminal));
    const cJSON *operation = cJSON_GetObjectItemCaseSensitive(response, "operation");
    return string_field(operation, "operation_id", terminal->operation_id, sizeof(terminal->operation_id)) &&
           string_field(operation, "group_id", terminal->group_id, sizeof(terminal->group_id)) &&
           json_version(operation, "expected_version", &terminal->expected_version) &&
           string_field(operation, "status", terminal->status, sizeof(terminal->status)) &&
           string_field(operation, "error_code", terminal->error_code, sizeof(terminal->error_code)) &&
           string_field(operation, "current_group_id", terminal->current_group_id, sizeof(terminal->current_group_id)) &&
           json_version(operation, "current_version", &terminal->current_version) &&
           group_switch_terminal_matches(&pending_operation, terminal);
}

static esp_err_t resolve_pending_switch(uint32_t session)
{
    if (journal_blocked || !pending_saved) {
        (void)update_ui("切群记录不可读取，音频保持暂停");
        return ESP_ERR_INVALID_STATE;
    }
    if (!group_switch_journal_bound(&pending_operation, config.server_url, config.device_id)) {
        portENTER_CRITICAL(&guard);
        switch_phase = SWITCH_MISMATCH;
        portEXIT_CRITICAL(&guard);
        (void)update_ui("有未确认的切群，请恢复原服务与身份配置");
        return ESP_ERR_INVALID_STATE;
    }
    if (!switch_session_active(session)) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&guard);
    switch_phase = SWITCH_CONFIRMING;
    portEXIT_CRITICAL(&guard);
    char path[112];
    snprintf(path, sizeof(path), "/operations/%s", pending_operation.operation_id);
    cJSON *out = NULL;
    esp_err_t err = request(path, HTTP_METHOD_GET, NULL, &out);
    if (err != ESP_OK && last_http_status == 404 && switch_session_active(session)) {
        cJSON *body = cJSON_CreateObject();
        if (!body || !cJSON_AddStringToObject(body, "operation_id", pending_operation.operation_id) ||
            !cJSON_AddStringToObject(body, "group_id", pending_operation.group_id) ||
            !cJSON_AddNumberToObject(body, "expected_version", (double)pending_operation.expected_version)) {
            cJSON_Delete(body);
            return ESP_ERR_NO_MEM;
        }
        /* Never change this ID, target, or version after a timeout, reconnect,
         * STOP/START, reboot, or server_epoch change. */
        err = request("/current-group", HTTP_METHOD_PUT, body, &out);
        cJSON_Delete(body);
    }
    group_switch_terminal_t terminal;
    if (err == ESP_OK && !parse_switch_terminal(out, &terminal)) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    cJSON_Delete(out);
    if (err != ESP_OK) {
        (void)update_ui("切群结果尚未确认，音频暂停并重试同一操作");
        return err;
    }
    /* The immutable operation snapshot may be older than the current state.
     * A fresh state is required even when PUT already returned a state member. */
    cJSON *state = NULL;
    const int64_t started = esp_timer_get_time();
    err = request("/state", HTTP_METHOD_GET, NULL, &state);
    char current[81];
    uint64_t current_version;
    if (err == ESP_OK && (!string_field(state, "current_group_id", current, sizeof(current)) ||
        !json_version(state, "current_version", &current_version) ||
        !group_switch_state_confirms(&terminal, current, current_version))) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK) {
        err = apply_state(state, started, esp_timer_get_time());
    }
    cJSON_Delete(state);
    if (err != ESP_OK) {
        (void)update_ui("切群终态已收到，仍在确认当前群，音频暂停");
        return err;
    }
    err = group_switch_journal_clear(pending_operation.operation_id);
    if (err != ESP_OK) {
        (void)update_ui("正在保存切群确认结果，音频保持暂停");
        return err;
    }
    pending_saved = false;
    memset(&pending_operation, 0, sizeof(pending_operation));
    portENTER_CRITICAL(&guard);
    intent_cancel(&intent);
    intent.ready = false;
    intent.switch_pending = false;
    switch_phase = SWITCH_IDLE;
    switch_operation_id[0] = 0;
    switch_target_id[0] = 0;
    advance_view_locked();
    portEXIT_CRITICAL(&guard);
    (void)update_ui(!strcmp(terminal.status, "applied") ? "切群已确认，正在按当前群恢复连接" :
                                                        "切群被拒绝，正在按服务端当前群恢复");
    return ESP_OK;
}

/* Called on the peer worker: copy only the first current final, never do I/O. */
static void command_event(const webrtc_command_event_t *event, void *context)
{
    (void)context;
    if (!event) return;
    portENTER_CRITICAL(&guard);
    if (event->type == WEBRTC_COMMAND_READY) {
        (void)group_command_ready(&command, event->generation);
    } else if (event->type == WEBRTC_COMMAND_FINAL) {
        (void)group_command_final(&command, event->generation, event->text);
    } else if (event->type == WEBRTC_COMMAND_ERROR) {
        (void)group_command_error(&command, event->generation);
    }
    portEXIT_CRITICAL(&guard);
    notify_worker();
}

static void finish_command(const char *detail)
{
    portENTER_CRITICAL(&guard);
    group_command_finish(&command);
    intent.command_busy = false;
    intent_cancel(&intent);
    portEXIT_CRITICAL(&guard);
    command_target[0] = 0;
    command_deadline_us = command_cleanup_retry_us = 0;
    (void)update_ui(detail);
}

static void join_command(void)
{
    char code[9];
    portENTER_CRITICAL(&guard);
    const bool active = intent.desired && command.kind == GROUP_COMMAND_JOIN;
    memcpy(code, command.code, sizeof(code));
    memset(command.code, 0, sizeof(command.code));
    group_command_phase(&command, GROUP_COMMAND_STARTING);
    portEXIT_CRITICAL(&guard);
    if (!active) { mbedtls_platform_zeroize(code, sizeof(code)); finish_command("已停止，入群尚未提交"); return; }
    release_floor();
    (void)update_ui("正在确认入群口令");
    cJSON *body = cJSON_CreateObject(), *out = NULL;
    esp_err_t err = ESP_ERR_NO_MEM;
    int membership_status = 0;
    const int64_t started = esp_timer_get_time();
    if (body && cJSON_AddStringToObject(body, "code", code)) {
        portENTER_CRITICAL(&guard);
        const bool submit = intent.desired && command.kind == GROUP_COMMAND_JOIN;
        if (submit) { command.submitted = true; ++command.revision; }
        portEXIT_CRITICAL(&guard);
        err = submit ? request("/memberships", HTTP_METHOD_POST, body, &out) : ESP_ERR_INVALID_STATE;
        membership_status = last_http_status;
        cJSON *value = cJSON_GetObjectItemCaseSensitive(body, "code");
        mbedtls_platform_zeroize(value->valuestring, strlen(value->valuestring));
    }
    mbedtls_platform_zeroize(code, sizeof(code));
    cJSON_Delete(body);
    if (err == ESP_OK) err = apply_state(cJSON_GetObjectItemCaseSensitive(out, "state"), started, esp_timer_get_time());
    cJSON_Delete(out);
    if (err != ESP_OK) {
        /* A lost POST response can still have added membership/current group.
         * Retire old media and require a fresh state before any new capture. */
        disconnect_peer();
        (void)reject_state(err);
        finish_command(membership_status == 429 ? "口令尝试较频繁，请稍后重试" : "入群尚未确认，请检查口令或连接后重试");
        return;
    }
    finish_command("入群已确认，新加入群已保存，可从群列表选择进入");
}

/* True means this iteration belongs exclusively to the RTA lifecycle. A
 * recovering command allows the existing journal/state/RTC worker to proceed. */
static bool process_command(void)
{
    portENTER_CRITICAL(&guard);
    group_command_state_t current = command;
    const bool active = intent.desired;
    const bool lost = intent.link_lost;
    const bool ready = intent.ready;
    portEXIT_CRITICAL(&guard);
    if (current.kind == GROUP_COMMAND_NONE) return false;
    if (current.kind == GROUP_COMMAND_JOIN) { join_command(); return true; }
    if (current.phase == GROUP_COMMAND_RECOVERING) {
        if (!active || ready) finish_command(active ? (command_result[0] ? command_result : "语音操作结束，当前群音频已就绪") : "已停止，未确认的切群将在下次连接时继续");
        return false;
    }
    if (current.phase == GROUP_COMMAND_QUEUED) {
        command_result[0] = command_target[0] = 0;
        command_deadline_us = esp_timer_get_time() + 45000000;
        command_cleanup_retry_us = 0;
    }
    if ((!active || lost || (command_deadline_us && esp_timer_get_time() >= command_deadline_us)) && !current.cancelled) {
        portENTER_CRITICAL(&guard);
        (void)group_command_cancel(&command);
        portEXIT_CRITICAL(&guard);
        webrtc_peer_command_cancel();
        strlcpy(command_result, active ? "语音指令已取消或超时，已恢复当前群" : "已停止语音指令", sizeof(command_result));
        current.cancelled = true;
    }
    if (current.cancelled && current.phase != GROUP_COMMAND_CLEANUP) {
        portENTER_CRITICAL(&guard);
        group_command_phase(&command, GROUP_COMMAND_CLEANUP);
        portEXIT_CRITICAL(&guard);
        current.phase = GROUP_COMMAND_CLEANUP;
    }
    if (current.phase == GROUP_COMMAND_QUEUED) {
        disconnect_peer();
        portENTER_CRITICAL(&guard);
        if (command.cancelled || !intent.desired) {
            group_command_phase(&command, GROUP_COMMAND_CLEANUP);
            portEXIT_CRITICAL(&guard);
            return true;
        }
        group_command_phase(&command, GROUP_COMMAND_STARTING);
        portEXIT_CRITICAL(&guard);
        (void)update_ui("正在准备语音指令，暂时无法收听群聊");
        memset(&command_session, 0, sizeof(command_session));
        esp_err_t err = rti_session_create_command(&command_session);
        /* Even a malformed create response may identify a live remote session.
         * Keep that handle for strict terminal cleanup before group recovery. */
        command_session_live = command_session.session_id[0] != 0;
        portENTER_CRITICAL(&guard);
        bool start = err == ESP_OK && intent.desired && !intent.link_lost && !command.cancelled && command.ticket == current.ticket;
        portEXIT_CRITICAL(&guard);
        if (start) {
            command_peer_live = true; /* stop also cleans a partial failed start */
            err = webrtc_peer_start_command(&command_session, &audio, current.ticket, command_event, NULL);
        }
        if (err != ESP_OK || !start) {
            portENTER_CRITICAL(&guard);
            (void)group_command_error(&command, current.ticket);
            portEXIT_CRITICAL(&guard);
            strlcpy(command_result, "语音指令未执行，已恢复当前群，请重试", sizeof(command_result));
        }
        return true;
    }
    if (current.phase == GROUP_COMMAND_STARTING || current.phase == GROUP_COMMAND_LISTENING) {
        (void)update_ui(current.phase == GROUP_COMMAND_LISTENING ? "请说：切到某某群；完成后会自动恢复对讲" : "正在等待语音通道就绪，暂时无法收听群聊");
        return true;
    }
    if (current.phase == GROUP_COMMAND_RESOLVING) {
        (void)update_ui("已收到语音，正在核对已加入群");
        const int64_t resolve_started = esp_timer_get_time();
        const size_t input_bytes = strlen(current.transcript);
        bool requested = false;
        int resolve_status = 0;
        group_voice_api_error_t api_error = GROUP_VOICE_API_NONE;
        cJSON *body = cJSON_CreateObject(), *out = NULL;
        esp_err_t err = ESP_ERR_NO_MEM;
        if (body && cJSON_AddStringToObject(body, "transcript", current.transcript)) {
            requested = true;
            err = request("/voice/resolve", HTTP_METHOD_POST, body, &out);
            resolve_status = last_http_status;
            api_error = last_voice_api_error;
            cJSON *text = cJSON_GetObjectItemCaseSensitive(body, "transcript");
            mbedtls_platform_zeroize(text->valuestring, strlen(text->valuestring));
        }
        mbedtls_platform_zeroize(current.transcript, sizeof(current.transcript));
        cJSON_Delete(body);
        char target[81] = {0};
        bool member = false;
        const bool target_valid = err == ESP_OK && string_field(out, "group_id", target, sizeof(target)) &&
                                  group_switch_identifier(target);
        if (target_valid) {
            for (size_t i = 0; i < catalog_count; ++i) if (!strcmp(catalog_items[i].id, target)) member = true;
        }
        cJSON_Delete(out);
        portENTER_CRITICAL(&guard);
        const bool cancelled = command.cancelled || !intent.desired;
        const bool selected = !command.cancelled && !command.failed && intent.desired && member;
        if (!command.cancelled && !command.failed && intent.desired && member) strlcpy(command_target, target, sizeof(command_target));
        else {
            command_target[0] = 0;
            if (!command.cancelled) command.failed = true;
        }
        memset(command.transcript, 0, sizeof(command.transcript));
        group_command_phase(&command, GROUP_COMMAND_CLEANUP);
        portEXIT_CRITICAL(&guard);
        const group_voice_outcome_t outcome = group_voice_classify(requested, err == ESP_OK,
            resolve_status, api_error, target_valid, member, cancelled);
        ESP_LOGI(TAG, "GROUP_VOICE_RESOLVE ticket=%" PRIu32 " input_bytes=%u requested=%d http=%d err=%d api=%s outcome=%s target_valid=%d member=%d selected=%d elapsed_ms=%" PRId64,
                 current.ticket, (unsigned)input_bytes, requested, resolve_status, (int)err,
                 group_voice_api_error_name(api_error), group_voice_outcome_name(outcome),
                 target_valid, member, selected, (esp_timer_get_time() - resolve_started) / 1000);
        const char *detail = group_voice_outcome_detail(outcome);
        if (detail) strlcpy(command_result, detail, sizeof(command_result));
        return true;
    }
    if (current.phase == GROUP_COMMAND_CLEANUP) {
        /* Cancel may precede QUEUED processing, when the old group Peer still
         * exists. Retire it before a future start can overwrite its peer_id. */
        if (peer_id[0] || floor_request) disconnect_peer();
        webrtc_peer_command_cancel();
        if (command_peer_live) { webrtc_peer_stop(); command_peer_live = false; }
        if (command_session_live) {
            if (esp_timer_get_time() < command_cleanup_retry_us) return true;
            (void)update_ui("正在结束语音会话，确认后恢复群音频");
            const esp_err_t err = rti_session_stop_command(&command_session, "group_command_finished");
            if (err != ESP_OK) {
                command_cleanup_retry_us = esp_timer_get_time() + 2000000;
                (void)update_ui("语音会话结束尚未确认，麦克风已停，正在重试");
                return true;
            }
            command_session_live = false;
        }
        mbedtls_platform_zeroize(&command_session, sizeof(command_session));
        portENTER_CRITICAL(&guard);
        intent.command_busy = false;
        const bool submit = command_target[0] && !command.cancelled && !command.failed && intent.desired && !intent.link_lost;
        if (submit && strcmp(command_target, published_group_id) &&
            intent_queue_switch(&intent, catalog_known, view_generation, view_generation)) {
            strlcpy(selected_group_id, command_target, sizeof(selected_group_id));
            selected_expected_version = command_expected_version;
            selection_queued = true;
            switch_phase = SWITCH_QUEUED;
            switch_operation_id[0] = 0;
            strlcpy(switch_target_id, command_target, sizeof(switch_target_id));
            command.submitted = true;
        }
        group_command_phase(&command, GROUP_COMMAND_RECOVERING);
        intent_cancel(&intent);
        intent.ready = false;
        portEXIT_CRITICAL(&guard);
        if (current.cancelled && !command_result[0]) strlcpy(command_result, "语音操作已取消，已恢复当前群", sizeof(command_result));
        (void)update_ui("语音采集已结束，正在确认当前群并恢复音频");
        return false;
    }
    return true;
}

esp_err_t group_controller_join_group(const char *code, void *context)
{
    (void)context;
    char normalized[9];
    if (!group_command_code(code, normalized)) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&guard);
    const bool accepted = intent.desired && catalog_known && !intent.link_lost && !intent.switch_pending &&
        group_command_begin(&command, GROUP_COMMAND_JOIN, normalized);
    if (accepted) { cancel_locked(); intent.command_busy = true; }
    portEXIT_CRITICAL(&guard);
    mbedtls_platform_zeroize(normalized, sizeof(normalized));
    if (accepted) notify_worker();
    return accepted ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t group_controller_voice_action(bool cancel, void *context)
{
    (void)context;
    portENTER_CRITICAL(&guard);
    bool accepted;
    if (cancel) {
        accepted = group_command_cancel(&command);
        if (accepted) webrtc_peer_command_cancel();
    } else {
        accepted = intent.desired && intent.ready && catalog_known && published_group_id[0] && !intent.link_lost &&
            !intent.switch_pending && group_command_begin(&command, GROUP_COMMAND_VOICE, NULL);
        if (accepted) {
            cancel_locked();
            webrtc_peer_group_suspend_media();
            intent.ready = false;
            intent.command_busy = true;
            command_expected_version = published_version;
        }
    }
    portEXIT_CRITICAL(&guard);
    if (accepted) notify_worker();
    return accepted ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static void run(void *arg)
{
    (void)arg;
    int64_t retry_at = 0;
    bool configured = false;
    bool stopped_published = false;
    uint32_t configured_session = 0;
    reload_switch_journal();
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        portENTER_CRITICAL(&guard);
        group_intent_t snapshot = intent;
        int64_t deadline = lease_deadline_us;
        bool queued = selection_queued;
        portEXIT_CRITICAL(&guard);
        if (process_command()) { continue; }
        /* Command cleanup may enqueue the durable switch or finish recovery.
         * Never use a pre-command snapshot to enter journal resolution. */
        portENTER_CRITICAL(&guard);
        snapshot = intent;
        deadline = lease_deadline_us;
        queued = selection_queued;
        portEXIT_CRITICAL(&guard);
        if (queued && configured) {
            /* Persist an accepted selection under its original identity before
             * a STOP/config reload can wipe that identity from RAM. */
            prepare_selected_switch();
            retry_at = 0;
            continue;
        }
        if (!snapshot.desired) {
            if (configured || peer_id[0] || floor_request) {
                disconnect_peer();
                mbedtls_platform_zeroize(&config, sizeof(config));
                configured = false;
                forget_catalog();
            }
            if (!stopped_published) {
                stopped_published = update_ui(snapshot.switch_pending ?
                    "已停止，未确认的切群会在下次连接时继续" : "已停止，点击连接群音频");
            }
            retry_at = 0;
            continue;
        }
        stopped_published = false;
        if (!configured || configured_session != snapshot.session) {
            if (configured || peer_id[0] || floor_request) {
                disconnect_peer();
                mbedtls_platform_zeroize(&config, sizeof(config));
                configured = false;
            }
            forget_catalog();
            if (esp_timer_get_time() < retry_at) {
                continue;
            }
            if (group_config_load(&config) != ESP_OK) {
                (void)update_ui("请通过 USB 配置群服务");
                retry_at = esp_timer_get_time() + 3000000;
                continue;
            }
            configured = true;
            configured_session = snapshot.session;
            legacy_join_attempted = false;
            reload_switch_journal();
        }
        if (!trusted_clock()) {
            if (peer_id[0] || floor_request) {
                disconnect_peer();
            }
            drop_http_client();
            (void)update_ui("正在同步网络时间；无外网时可用 USB 同步");
            continue;
        }
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
            if (peer_id[0] || floor_request) {
                disconnect_peer();
            }
            drop_http_client();
            (void)update_ui("等待 Wi-Fi 恢复");
            continue;
        }
        portENTER_CRITICAL(&guard);
        const bool switching = intent.switch_pending;
        portEXIT_CRITICAL(&guard);
        if (switching) {
            if (esp_timer_get_time() < retry_at) {
                continue;
            }
            const esp_err_t err = resolve_pending_switch(configured_session);
            retry_at = err == ESP_OK ? 0 : esp_timer_get_time() + 1800000;
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
            (void)update_ui("正在连接群服务");
            const esp_err_t err = connect_peer(configured_session);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "group connect failed: %s HTTP=%d", esp_err_to_name(err), last_http_status);
                (void)update_ui(err == ESP_ERR_NOT_FOUND ? "尚未加入群组，点击加入群组输入口令" : "群连接失败，请检查服务或重试");
                retry_at = esp_timer_get_time() + 3000000;
            }
            continue;
        }
        if (!webrtc_peer_group_connected()) {
            disconnect_peer();
            retry_at = esp_timer_get_time() + 1000000;
            (void)update_ui("群音频连接中断，已停发");
            continue;
        }
        if (floor_request && (!snapshot.pressed || snapshot.generation != request_generation ||
                              (snapshot.held && esp_timer_get_time() >= deadline))) {
            release_floor();
            (void)update_ui("群音频就绪，按住说话");
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
                (void)update_ui("控制连接中断，已停发");
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
    } else if (command.kind == GROUP_COMMAND_NONE) {
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
        (void)group_command_cancel(&command);
        webrtc_peer_command_cancel();
        webrtc_peer_group_suspend_media();
        catalog_known = false;
        advance_view_locked();
    }
    intent_action(&intent, stop);
    portEXIT_CRITICAL(&guard);
    notify_worker();
}

esp_err_t group_controller_switch_group(const char *target_group_id, uint32_t selected_view, void *context)
{
    (void)context;
    if (!group_switch_identifier(target_group_id)) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&guard);
    if (!intent.desired || !catalog_known || intent.link_lost || intent.switch_pending || command.kind != GROUP_COMMAND_NONE ||
        selected_view != view_generation) {
        portEXIT_CRITICAL(&guard);
        return ESP_ERR_INVALID_STATE;
    }
    if (!strcmp(target_group_id, published_group_id)) {
        portEXIT_CRITICAL(&guard);
        return ESP_OK;
    }
    webrtc_peer_group_suspend_media();
    const bool queued = intent_queue_switch(&intent, catalog_known, view_generation, selected_view);
    if (queued) {
        strlcpy(selected_group_id, target_group_id, sizeof(selected_group_id));
        selected_expected_version = published_version;
        selection_queued = true;
        switch_phase = SWITCH_QUEUED;
        switch_operation_id[0] = 0;
        strlcpy(switch_target_id, target_group_id, sizeof(switch_target_id));
    }
    portEXIT_CRITICAL(&guard);
    notify_worker();
    return queued ? ESP_OK : ESP_ERR_INVALID_STATE;
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
    catalog_lock = xSemaphoreCreateMutex();
    if (!catalog_lock) {
        return ESP_ERR_NO_MEM;
    }
    audio = *board;
    const uint8_t initial_volume = board_audio_get_output_volume(&audio);
    esp_err_t err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = board_display_show_group(group_controller_action, group_controller_ptt, group_controller_switch_group, volume, initial_volume, NULL);
    if (err == ESP_OK) {
        err = board_display_set_group_commands(group_controller_join_group, group_controller_voice_action, NULL);
    }
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

/* Serial callers copy the worker-owned catalog under its mutex. Formatting and
 * UART output happen after both locks are released, and large buffers use PSRAM. */
typedef struct {
    board_group_item_t items[GROUP_CHOICES_MAX];
    size_t count;
    char current[81];
    uint32_t view;
    uint64_t version;
    bool known;
    bool switching;
} group_catalog_snapshot_t;

static bool json_append(char *out, size_t capacity, size_t *used, const char *text)
{
    const size_t length = strlen(text);
    if (*used >= capacity || length >= capacity - *used) {
        return false;
    }
    memcpy(out + *used, text, length + 1);
    *used += length;
    return true;
}

static bool json_append_string(char *out, size_t capacity, size_t *used, const char *text)
{
    static const char hex[] = "0123456789abcdef";
    if (!json_append(out, capacity, used, "\"")) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        char encoded[7] = {0};
        if (*p == '"' || *p == '\\') {
            encoded[0] = '\\';
            encoded[1] = (char)*p;
        } else if (*p < 0x20) {
            memcpy(encoded, "\\u00", 4);
            encoded[4] = hex[*p >> 4];
            encoded[5] = hex[*p & 15];
        } else {
            encoded[0] = (char)*p;
        }
        if (!json_append(out, capacity, used, encoded)) {
            return false;
        }
    }
    return json_append(out, capacity, used, "\"");
}

void group_controller_print_groups(void)
{
    enum { JSON_CAPACITY = 48 * 1024 };
    group_catalog_snapshot_t *snapshot = heap_caps_calloc(1, sizeof(*snapshot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *json = heap_caps_malloc(JSON_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!snapshot || !json) {
        free(snapshot);
        free(json);
        printf("INTERCOM_GROUPS_ERROR no_memory\n");
        fflush(stdout);
        return;
    }
    if (!catalog_lock || xSemaphoreTake(catalog_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        free(snapshot);
        free(json);
        printf("INTERCOM_GROUPS_ERROR catalog_busy\n");
        fflush(stdout);
        return;
    }
    snapshot->count = catalog_count;
    if (snapshot->count) {
        memcpy(snapshot->items, catalog_items, snapshot->count * sizeof(snapshot->items[0]));
    }
    portENTER_CRITICAL(&guard);
    snapshot->view = view_generation;
    snapshot->version = published_version;
    snapshot->known = catalog_known;
    snapshot->switching = intent.switch_pending;
    strlcpy(snapshot->current, published_group_id, sizeof(snapshot->current));
    portEXIT_CRITICAL(&guard);
    xSemaphoreGive(catalog_lock);
    const int prefix = snprintf(json, JSON_CAPACITY,
        "{\"view_generation\":%" PRIu32 ",\"current_group_id\":\"%s\",\"current_version\":%" PRIu64
        ",\"known\":%s,\"switching\":%s,\"groups\":[", snapshot->view, snapshot->current, snapshot->version,
        snapshot->known ? "true" : "false", snapshot->switching ? "true" : "false");
    size_t used = prefix > 0 ? (size_t)prefix : JSON_CAPACITY;
    bool complete = used < JSON_CAPACITY;
    for (size_t i = 0; complete && i < snapshot->count; ++i) {
        complete = json_append(json, JSON_CAPACITY, &used, i ? ",{\"id\":" : "{\"id\":") &&
                   json_append_string(json, JSON_CAPACITY, &used, snapshot->items[i].id) &&
                   json_append(json, JSON_CAPACITY, &used, ",\"name\":") &&
                   json_append_string(json, JSON_CAPACITY, &used, snapshot->items[i].name) &&
                   json_append(json, JSON_CAPACITY, &used, "}");
    }
    complete = complete && json_append(json, JSON_CAPACITY, &used, "]}");
    if (complete) {
        printf("INTERCOM_GROUPS %s\n", json);
    } else {
        /* Never silently omit groups, even for unusually escape-heavy names. */
        printf("INTERCOM_GROUPS_ERROR response_too_large\n");
    }
    free(json);
    free(snapshot);
    fflush(stdout);
}

void group_controller_print_status(void)
{
    portENTER_CRITICAL(&guard);
    const group_intent_t snapshot = intent;
    const int64_t deadline = lease_deadline_us;
    const uint64_t requests = http_requests;
    const uint64_t connections = tls_connections;
    const uint64_t failures = http_failures;
    const bool known = catalog_known;
    const uint32_t view = view_generation;
    const uint64_t current_version = published_version;
    const size_t group_count = published_group_count;
    char current[81], operation[81], target[81];
    strlcpy(current, published_group_id, sizeof(current));
    strlcpy(operation, switch_operation_id, sizeof(operation));
    strlcpy(target, switch_target_id, sizeof(target));
    const switch_phase_t phase = switch_phase;
    const unsigned command_kind = (unsigned)command.kind, command_phase = (unsigned)command.phase;
    const uint32_t command_revision = command.revision;
    portEXIT_CRITICAL(&guard);
    const bool connected = snapshot.ready && webrtc_peer_group_connected();
    const bool holding = connected && snapshot.held && snapshot.pressed && esp_timer_get_time() < deadline;
    webrtc_group_stats_t stats = {0};
    webrtc_peer_group_get_stats(&stats);
    printf("INTERCOM_STATUS {\"configured\":%s,\"desired\":%s,\"connected\":%s,\"pressed\":%s,\"held\":%s,\"busy\":%s,"
           "\"source_frames\":%" PRIu64 ",\"sent_packets\":%" PRIu64 ",\"received_packets\":%" PRIu64 ",\"decoded_samples\":%" PRIu64 ","
           "\"source_rms_peak\":%.6f,\"remote_rms_peak\":%.6f,\"output_volume\":%u,"
           "\"ptt_id\":%" PRIu32 ",\"ptt_start_us\":%" PRId64 ",\"ptt_end_us\":%" PRId64 ","
           "\"ptt_raw_n\":%" PRIu64 ",\"ptt_raw_peak\":%.6f,\"ptt_raw_mean\":%.6f,"
           "\"ptt_enc_n\":%" PRIu64 ",\"ptt_enc_peak\":%.6f,\"ptt_enc_mean\":%.6f,\"heap_internal_free\":%u,"
           "\"http_requests\":%" PRIu64 ",\"tls_connections\":%" PRIu64 ",\"http_failures\":%" PRIu64 ","
           "\"catalog_known\":%s,\"view_generation\":%" PRIu32 ",\"current_group_id\":\"%s\",\"current_version\":%" PRIu64 ","
           "\"group_count\":%u,\"switch_pending\":%s,\"switch_phase\":%u,\"switch_operation_id\":\"%s\",\"target_group_id\":\"%s\",\"command_kind\":%u,\"command_phase\":%u,\"command_revision\":%" PRIu32 "}\n",
           group_config_ready() ? "true" : "false", snapshot.desired ? "true" : "false", connected ? "true" : "false",
           snapshot.pressed ? "true" : "false", holding ? "true" : "false", snapshot.busy ? "true" : "false",
           stats.source_frames, stats.sent_packets, stats.received_packets, stats.decoded_samples,
           (double)stats.source_rms_peak, (double)stats.remote_rms_peak, (unsigned)board_audio_get_output_volume(&audio),
           stats.ptt_id, stats.ptt_start_us, stats.ptt_end_us,
           stats.ptt_raw_frames, (double)stats.ptt_raw_rms_peak,
           stats.ptt_raw_frames ? stats.ptt_raw_rms_sum / stats.ptt_raw_frames : 0.0,
           stats.ptt_encoder_frames, (double)stats.ptt_encoder_rms_peak,
           stats.ptt_encoder_frames ? stats.ptt_encoder_rms_sum / stats.ptt_encoder_frames : 0.0,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), requests, connections, failures,
           known ? "true" : "false", view, current, current_version, (unsigned)group_count,
           snapshot.switch_pending ? "true" : "false", (unsigned)phase, operation, target, command_kind, command_phase, command_revision);
    fflush(stdout);
}
#endif

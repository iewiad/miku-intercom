#include "group_switch_journal.h"

#include <stddef.h>
#include <string.h>

static bool terminated(const char *value, size_t capacity)
{
    return value && memchr(value, '\0', capacity) != NULL;
}

bool group_switch_identifier(const char *value)
{
    if (!value) {
        return false;
    }
    const size_t size = strnlen(value, 81);
    if (size < 8 || size > 80) {
        return false;
    }
    for (size_t i = 0; i < size; ++i) {
        const unsigned char c = (unsigned char)value[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            return false;
        }
    }
    return true;
}

bool group_switch_journal_valid(const group_switch_journal_t *record)
{
    return record && record->format == GROUP_SWITCH_JOURNAL_FORMAT &&
           terminated(record->server_url, sizeof(record->server_url)) &&
           strncmp(record->server_url, "https://", 8) == 0 && record->server_url[8] &&
           terminated(record->device_id, sizeof(record->device_id)) &&
           terminated(record->operation_id, sizeof(record->operation_id)) &&
           terminated(record->group_id, sizeof(record->group_id)) &&
           group_switch_identifier(record->device_id) &&
           group_switch_identifier(record->operation_id) &&
           group_switch_identifier(record->group_id) &&
           record->expected_version <= GROUP_SWITCH_MAX_VERSION;
}

bool group_switch_journal_bound(const group_switch_journal_t *record,
                                 const char *server_url, const char *device_id)
{
    return group_switch_journal_valid(record) && server_url && device_id &&
           strcmp(record->server_url, server_url) == 0 && strcmp(record->device_id, device_id) == 0;
}

bool group_switch_terminal_matches(const group_switch_journal_t *record,
                                    const group_switch_terminal_t *terminal)
{
    if (!group_switch_journal_valid(record) || !terminal ||
        !terminated(terminal->operation_id, sizeof(terminal->operation_id)) ||
        !terminated(terminal->group_id, sizeof(terminal->group_id)) ||
        !terminated(terminal->status, sizeof(terminal->status)) ||
        !terminated(terminal->error_code, sizeof(terminal->error_code)) ||
        !terminated(terminal->current_group_id, sizeof(terminal->current_group_id)) ||
        strcmp(terminal->operation_id, record->operation_id) ||
        strcmp(terminal->group_id, record->group_id) ||
        terminal->expected_version != record->expected_version ||
        terminal->current_version > GROUP_SWITCH_MAX_VERSION ||
        (terminal->current_group_id[0] && !group_switch_identifier(terminal->current_group_id))) {
        return false;
    }
    if (strcmp(terminal->status, "applied") == 0) {
        return !terminal->error_code[0] && strcmp(terminal->current_group_id, record->group_id) == 0 &&
               (terminal->current_version == record->expected_version ||
                (record->expected_version < GROUP_SWITCH_MAX_VERSION &&
                 terminal->current_version == record->expected_version + 1));
    }
    return strcmp(terminal->status, "rejected") == 0 && terminal->error_code[0];
}

bool group_switch_state_confirms(const group_switch_terminal_t *terminal,
                                  const char *current_group_id, uint64_t current_version)
{
    return terminal && current_group_id && current_version <= GROUP_SWITCH_MAX_VERSION &&
           current_version >= terminal->current_version &&
           (current_version != terminal->current_version ||
            strcmp(current_group_id, terminal->current_group_id) == 0);
}

#ifndef GROUP_CONTROLLER_HOST_TEST
#include "nvs.h"

#define JOURNAL_NAMESPACE "intercom"
#define JOURNAL_KEY "pending_switch"

esp_err_t group_switch_journal_load(group_switch_journal_t *record)
{
    if (!record) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(record, 0, sizeof(*record));
    nvs_handle_t handle;
    esp_err_t err = nvs_open(JOURNAL_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        return err;
    }
    size_t size = sizeof(*record);
    err = nvs_get_blob(handle, JOURNAL_KEY, record, &size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_ERR_NOT_FOUND;
    } else if (err == ESP_OK && (size != sizeof(*record) || !group_switch_journal_valid(record))) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err != ESP_OK) {
        memset(record, 0, sizeof(*record));
    }
    return err;
}

esp_err_t group_switch_journal_save(const group_switch_journal_t *record)
{
    if (!group_switch_journal_valid(record)) {
        return ESP_ERR_INVALID_ARG;
    }
    group_switch_journal_t old;
    esp_err_t err = group_switch_journal_load(&old);
    if (err == ESP_OK) {
        return strcmp(old.server_url, record->server_url) == 0 &&
               strcmp(old.device_id, record->device_id) == 0 &&
               strcmp(old.operation_id, record->operation_id) == 0 &&
               strcmp(old.group_id, record->group_id) == 0 &&
               old.expected_version == record->expected_version ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (err != ESP_ERR_NOT_FOUND) {
        return err;
    }
    nvs_handle_t handle;
    err = nvs_open(JOURNAL_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, JOURNAL_KEY, record, sizeof(*record));
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    return err;
}

esp_err_t group_switch_journal_clear(const char *expected_operation_id)
{
    group_switch_journal_t old;
    esp_err_t err = group_switch_journal_load(&old);
    if (err == ESP_ERR_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (!expected_operation_id || strcmp(old.operation_id, expected_operation_id)) {
        return ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t handle;
    err = nvs_open(JOURNAL_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_erase_key(handle, JOURNAL_KEY);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    return err;
}
#endif

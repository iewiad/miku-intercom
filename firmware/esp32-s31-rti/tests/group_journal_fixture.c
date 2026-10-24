/* Test-only SDK double. Its transactions model error outcomes, not physical
 * NVS durability or power-loss behavior. All identities are fake fixture data.
 * The runner compiles the actual production journal as a separate translation
 * unit, without the production module's HOST_TEST exclusion. */
#include "group_switch_journal.h"
#include "nvs.h"
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BLOB_CAPACITY (sizeof(group_switch_journal_t) + 32)
#define HANDLE_COUNT 4

typedef struct {
    bool active;
    nvs_open_mode_t mode;
    bool dirty;
    bool erase;
    size_t size;
    unsigned char data[BLOB_CAPACITY];
} handle_t;

typedef enum { COMMIT_OK, COMMIT_FAIL_BEFORE, COMMIT_FAIL_AFTER } commit_mode_t;
static struct {
    bool namespace_exists;
    bool present;
    size_t size;
    unsigned char data[BLOB_CAPACITY];
} durable;
static handle_t handles[HANDLE_COUNT];
static commit_mode_t commit_mode;
static esp_err_t open_error, get_error, set_error, erase_error;
static unsigned opens, closes, sets, erases, commits, published;

static void require_no_handles(void)
{
    assert(opens == closes);
    for (size_t i = 0; i < HANDLE_COUNT; ++i) {
        assert(!handles[i].active);
    }
}

static void reset_fixture(void)
{
    require_no_handles();
    memset(&durable, 0, sizeof(durable));
    memset(handles, 0, sizeof(handles));
    commit_mode = COMMIT_OK;
    open_error = get_error = set_error = erase_error = ESP_OK;
    opens = closes = sets = erases = commits = published = 0;
}

static handle_t *get_handle(nvs_handle_t handle)
{
    assert(handle > 0 && handle <= HANDLE_COUNT);
    handle_t *value = &handles[handle - 1];
    assert(value->active);
    return value;
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out)
{
    assert(strcmp(name, "intercom") == 0);
    if (open_error != ESP_OK) {
        return open_error;
    }
    if (!durable.namespace_exists && mode == NVS_READONLY) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (mode == NVS_READWRITE) {
        durable.namespace_exists = true;
    }
    for (size_t i = 0; i < HANDLE_COUNT; ++i) {
        if (!handles[i].active) {
            memset(&handles[i], 0, sizeof(handles[i]));
            handles[i].active = true;
            handles[i].mode = mode;
            *out = (nvs_handle_t)(i + 1);
            ++opens;
            return ESP_OK;
        }
    }
    assert(false && "leaked NVS handles");
    return ESP_ERR_NVS_INVALID_HANDLE;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *size)
{
    (void)get_handle(handle);
    assert(strcmp(key, "pending_switch") == 0 && size);
    if (get_error != ESP_OK) {
        return get_error;
    }
    if (!durable.present) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (!out) {
        *size = durable.size;
        return ESP_OK;
    }
    if (*size < durable.size) {
        *size = durable.size;
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out, durable.data, durable.size);
    *size = durable.size;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t size)
{
    handle_t *transaction = get_handle(handle);
    assert(transaction->mode == NVS_READWRITE);
    assert(strcmp(key, "pending_switch") == 0 && value && size <= BLOB_CAPACITY);
    ++sets;
    if (set_error != ESP_OK) {
        return set_error;
    }
    transaction->dirty = true;
    transaction->erase = false;
    transaction->size = size;
    memcpy(transaction->data, value, size);
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    handle_t *transaction = get_handle(handle);
    assert(transaction->mode == NVS_READWRITE);
    assert(strcmp(key, "pending_switch") == 0);
    ++erases;
    if (erase_error != ESP_OK) {
        return erase_error;
    }
    if (!durable.present) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    transaction->dirty = true;
    transaction->erase = true;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    handle_t *transaction = get_handle(handle);
    assert(transaction->mode == NVS_READWRITE && transaction->dirty);
    ++commits;
    if (commit_mode == COMMIT_FAIL_BEFORE) {
        return ESP_FAIL;
    }
    durable.present = !transaction->erase;
    durable.size = transaction->erase ? 0 : transaction->size;
    memset(durable.data, 0, sizeof(durable.data));
    if (!transaction->erase) {
        memcpy(durable.data, transaction->data, transaction->size);
    }
    transaction->dirty = false;
    ++published;
    return commit_mode == COMMIT_FAIL_AFTER ? ESP_FAIL : ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    handle_t *transaction = get_handle(handle);
    /* This double discards uncommitted work. It also supports an error returned
     * after publication, so tests do not assume an error guarantees rollback. */
    memset(transaction, 0, sizeof(*transaction));
    ++closes;
}

static group_switch_journal_t sample_record(void)
{
    group_switch_journal_t record = {0};
    record.format = GROUP_SWITCH_JOURNAL_FORMAT;
    strcpy(record.server_url, "https://192.168.1.2:8443");
    strcpy(record.device_id, "device_fixture_A");
    strcpy(record.operation_id, "operation_fixture_001");
    strcpy(record.group_id, "group_fixture_target");
    record.expected_version = 7;
    return record;
}

static bool same_record(const group_switch_journal_t *a, const group_switch_journal_t *b)
{
    return a->format == b->format && a->expected_version == b->expected_version &&
           strcmp(a->server_url, b->server_url) == 0 &&
           strcmp(a->device_id, b->device_id) == 0 &&
           strcmp(a->operation_id, b->operation_id) == 0 &&
           strcmp(a->group_id, b->group_id) == 0;
}

static void assert_record(const group_switch_journal_t *expected)
{
    group_switch_journal_t actual;
    memset(&actual, 0xcc, sizeof(actual));
    assert(group_switch_journal_load(&actual) == ESP_OK);
    assert(same_record(&actual, expected));
    require_no_handles();
}

static void assert_cleared(const group_switch_journal_t *record)
{
    const group_switch_journal_t zero = {0};
    assert(memcmp(record, &zero, sizeof(zero)) == 0);
}

static void assert_no_record(void)
{
    group_switch_journal_t record;
    memset(&record, 0xcc, sizeof(record));
    assert(group_switch_journal_load(&record) == ESP_ERR_NOT_FOUND);
    assert_cleared(&record);
    require_no_handles();
}

static void inject_blob(const void *blob, size_t size)
{
    assert(size <= BLOB_CAPACITY);
    durable.namespace_exists = true;
    durable.present = true;
    durable.size = size;
    memset(durable.data, 0, sizeof(durable.data));
    memcpy(durable.data, blob, size);
}

static void test_save_load_clear(void)
{
    reset_fixture();
    const group_switch_journal_t expected = sample_record();
    assert(group_switch_journal_load(NULL) == ESP_ERR_INVALID_ARG);
    assert_no_record();
    assert(group_switch_journal_clear(expected.operation_id) == ESP_OK);
    assert(group_switch_journal_save(&expected) == ESP_OK);
    assert(sets == 1 && commits == 1 && published == 1);
    assert_record(&expected);
    /* Save is idempotent at semantic field level; trailing buffer bytes need
     * not match and must not cause a second persistent mutation. */
    group_switch_journal_t equivalent = expected;
    equivalent.group_id[sizeof(equivalent.group_id) - 1] = 'x';
    assert(group_switch_journal_save(&equivalent) == ESP_OK);
    assert(sets == 1 && commits == 1);
    assert_record(&expected);
    assert(group_switch_journal_clear(expected.operation_id) == ESP_OK);
    assert(erases == 1 && commits == 2 && published == 2);
    assert_no_record();
    assert(group_switch_journal_clear(expected.operation_id) == ESP_OK);
    assert(erases == 1 && commits == 2);
}

static void test_binding_and_conflicts(void)
{
    reset_fixture();
    const group_switch_journal_t expected = sample_record();
    assert(group_switch_journal_bound(&expected, expected.server_url, expected.device_id));
    assert(!group_switch_journal_bound(&expected, "https://192.168.1.3:8443", expected.device_id));
    assert(!group_switch_journal_bound(&expected, expected.server_url, "device_fixture_B"));
    assert(!group_switch_journal_bound(&expected, NULL, expected.device_id));
    assert(group_switch_journal_save(&expected) == ESP_OK);
    for (unsigned field = 0; field < 5; ++field) {
        group_switch_journal_t changed = expected;
        switch (field) {
        case 0: strcpy(changed.server_url, "https://192.168.1.3:8443"); break;
        case 1: strcpy(changed.device_id, "device_fixture_B"); break;
        case 2: strcpy(changed.operation_id, "operation_fixture_002"); break;
        case 3: strcpy(changed.group_id, "group_fixture_other"); break;
        case 4: ++changed.expected_version; break;
        }
        assert(group_switch_journal_save(&changed) == ESP_ERR_INVALID_STATE);
        assert_record(&expected);
    }
    assert(sets == 1 && commits == 1);
    assert(group_switch_journal_clear("operation_fixture_002") == ESP_ERR_INVALID_STATE);
    assert(group_switch_journal_clear(NULL) == ESP_ERR_INVALID_STATE);
    assert(erases == 0 && commits == 1);
    assert_record(&expected);
}

static void test_io_errors(void)
{
    reset_fixture();
    const group_switch_journal_t expected = sample_record();
    group_switch_journal_t loaded;
    open_error = ESP_FAIL;
    memset(&loaded, 0xcc, sizeof(loaded));
    assert(group_switch_journal_load(&loaded) == ESP_FAIL);
    assert_cleared(&loaded);
    assert(group_switch_journal_save(&expected) == ESP_FAIL);
    assert(sets == 0 && commits == 0);
    open_error = ESP_OK;
    set_error = ESP_FAIL;
    assert(group_switch_journal_save(&expected) == ESP_FAIL);
    assert(commits == 0);
    assert_no_record();
    set_error = ESP_OK;
    assert(group_switch_journal_save(&expected) == ESP_OK);
    get_error = ESP_FAIL;
    memset(&loaded, 0xcc, sizeof(loaded));
    assert(group_switch_journal_load(&loaded) == ESP_FAIL);
    assert_cleared(&loaded);
    assert(group_switch_journal_save(&expected) == ESP_FAIL);
    assert(group_switch_journal_clear(expected.operation_id) == ESP_FAIL);
    assert(sets == 2 && commits == 1);
    get_error = ESP_OK;
    assert_record(&expected);
    erase_error = ESP_FAIL;
    assert(group_switch_journal_clear(expected.operation_id) == ESP_FAIL);
    assert(commits == 1);
    assert_record(&expected);
}

static void test_commit_failures(void)
{
    const group_switch_journal_t expected = sample_record();
    for (unsigned after = 0; after < 2; ++after) {
        reset_fixture();
        commit_mode = after ? COMMIT_FAIL_AFTER : COMMIT_FAIL_BEFORE;
        assert(group_switch_journal_save(&expected) == ESP_FAIL);
        assert(commits == 1 && published == after);
        if (after) {
            assert_record(&expected);
        } else {
            assert_no_record();
        }
        commit_mode = COMMIT_OK;
        assert(group_switch_journal_save(&expected) == ESP_OK);
        assert_record(&expected);
        const unsigned commits_before_clear = commits;
        commit_mode = after ? COMMIT_FAIL_AFTER : COMMIT_FAIL_BEFORE;
        assert(group_switch_journal_clear(expected.operation_id) == ESP_FAIL);
        assert(commits == commits_before_clear + 1);
        if (after) {
            assert_no_record();
        } else {
            assert_record(&expected);
        }
        commit_mode = COMMIT_OK;
        assert(group_switch_journal_clear(expected.operation_id) == ESP_OK);
        assert_no_record();
    }
}

static void assert_bad_blob(void)
{
    group_switch_journal_t loaded;
    const group_switch_journal_t valid = sample_record();
    unsigned char before[BLOB_CAPACITY];
    memcpy(before, durable.data, sizeof(before));
    const size_t size = durable.size;
    memset(&loaded, 0xcc, sizeof(loaded));
    const esp_err_t err = group_switch_journal_load(&loaded);
    assert(err != ESP_OK && err != ESP_ERR_NOT_FOUND);
    assert_cleared(&loaded);
    assert(group_switch_journal_save(&valid) != ESP_OK);
    assert(group_switch_journal_clear(valid.operation_id) != ESP_OK);
    assert(sets == 0 && erases == 0 && commits == 0);
    assert(durable.present && durable.size == size && memcmp(before, durable.data, sizeof(before)) == 0);
    require_no_handles();
}

static void test_corrupted_blobs(void)
{
    const group_switch_journal_t valid = sample_record();
    for (unsigned variant = 0; variant < 11; ++variant) {
        reset_fixture();
        group_switch_journal_t corrupt = valid;
        switch (variant) {
        case 0:
            inject_blob(&corrupt, 1);
            break;
        case 1:
            inject_blob(&corrupt, sizeof(corrupt) - 1);
            break;
        case 2: {
            unsigned char oversized[BLOB_CAPACITY] = {0};
            memcpy(oversized, &corrupt, sizeof(corrupt));
            inject_blob(oversized, sizeof(corrupt) + 1);
            break;
        }
        case 3: corrupt.format = GROUP_SWITCH_JOURNAL_FORMAT + 1; break;
        case 4: memset(corrupt.server_url, 'x', sizeof(corrupt.server_url)); break;
        case 5: memset(corrupt.device_id, 'x', sizeof(corrupt.device_id)); break;
        case 6: memset(corrupt.operation_id, 'x', sizeof(corrupt.operation_id)); break;
        case 7: memset(corrupt.group_id, 'x', sizeof(corrupt.group_id)); break;
        case 8: corrupt.expected_version = GROUP_SWITCH_MAX_VERSION + 1; break;
        case 9: strcpy(corrupt.server_url, "http://192.168.1.2:8443"); break;
        case 10: strcpy(corrupt.operation_id, "invalid/id"); break;
        }
        if (variant >= 3) {
            inject_blob(&corrupt, sizeof(corrupt));
        }
        assert_bad_blob();
    }
    reset_fixture();
    group_switch_journal_t invalid = valid;
    invalid.format = 0;
    assert(group_switch_journal_save(NULL) == ESP_ERR_INVALID_ARG);
    assert(group_switch_journal_save(&invalid) == ESP_ERR_INVALID_ARG);
    assert(sets == 0 && commits == 0);
}

static group_switch_terminal_t sample_terminal(const group_switch_journal_t *record)
{
    group_switch_terminal_t terminal = {0};
    strcpy(terminal.operation_id, record->operation_id);
    strcpy(terminal.group_id, record->group_id);
    terminal.expected_version = record->expected_version;
    strcpy(terminal.status, "applied");
    strcpy(terminal.current_group_id, record->group_id);
    terminal.current_version = record->expected_version + 1;
    return terminal;
}

static void test_terminal_and_fresh_state(void)
{
    const group_switch_journal_t record = sample_record();
    const group_switch_terminal_t terminal = sample_terminal(&record);
    assert(group_switch_terminal_matches(&record, &terminal));
    assert(group_switch_state_confirms(&terminal, terminal.current_group_id, terminal.current_version));
    assert(!group_switch_state_confirms(&terminal, terminal.current_group_id, terminal.current_version - 1));
    assert(!group_switch_state_confirms(&terminal, "group_fixture_other", terminal.current_version));
    /* A later authoritative version may have legitimately switched again. */
    assert(group_switch_state_confirms(&terminal, "group_fixture_other", terminal.current_version + 1));
    assert(!group_switch_state_confirms(&terminal, terminal.current_group_id, GROUP_SWITCH_MAX_VERSION + 1));
    assert(!group_switch_state_confirms(NULL, terminal.current_group_id, terminal.current_version));
    assert(!group_switch_state_confirms(&terminal, NULL, terminal.current_version));
    for (unsigned field = 0; field < 9; ++field) {
        group_switch_terminal_t changed = terminal;
        switch (field) {
        case 0: strcpy(changed.operation_id, "operation_fixture_002"); break;
        case 1: strcpy(changed.group_id, "group_fixture_other"); break;
        case 2: ++changed.expected_version; break;
        case 3: strcpy(changed.status, "pending"); break;
        case 4: strcpy(changed.error_code, "unexpected_error"); break;
        case 5: strcpy(changed.current_group_id, "group_fixture_other"); break;
        case 6: ++changed.current_version; break;
        case 7: memset(changed.operation_id, 'x', sizeof(changed.operation_id)); break;
        case 8: memset(changed.current_group_id, 'x', sizeof(changed.current_group_id)); break;
        }
        assert(!group_switch_terminal_matches(&record, &changed));
    }
    group_switch_terminal_t rejected = terminal;
    strcpy(rejected.status, "rejected");
    strcpy(rejected.error_code, "version_conflict");
    strcpy(rejected.current_group_id, "group_fixture_original");
    assert(group_switch_terminal_matches(&record, &rejected));
    rejected.error_code[0] = 0;
    assert(!group_switch_terminal_matches(&record, &rejected));
    rejected = terminal;
    rejected.current_version = record.expected_version;
    assert(group_switch_terminal_matches(&record, &rejected));
}

int main(void)
{
    test_save_load_clear();
    test_binding_and_conflicts();
    test_io_errors();
    test_commit_failures();
    test_corrupted_blobs();
    test_terminal_and_fresh_state();
    require_no_handles();
    puts("production switch journal: passed (SDK double: storage, binding, commit errors, malformed blobs, terminal/state fences)");
    puts("scope: host simulation only; real NVS/Flash atomicity and power-loss durability were not tested");
    return 0;
}

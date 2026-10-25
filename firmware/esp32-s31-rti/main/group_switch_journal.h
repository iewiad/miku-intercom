#pragma once

#include <stdbool.h>
#include <stdint.h>

#define GROUP_SWITCH_JOURNAL_FORMAT 2U
#define GROUP_SWITCH_MAX_VERSION 9007199254740991ULL

typedef struct {
    uint32_t format;
    char server_url[192];
    char device_id[81];
    char operation_id[81];
    char group_id[81];
    uint64_t expected_version;
    char recommendation_id[81];
    char recommendation_epoch[81];
} group_switch_journal_t;

typedef struct {
    char operation_id[81];
    char group_id[81];
    uint64_t expected_version;
    char status[16];
    char error_code[81];
    char current_group_id[81];
    uint64_t current_version;
    char recommendation_id[81];
    char recommendation_epoch[81];
} group_switch_terminal_t;

bool group_switch_identifier(const char *value);
bool group_switch_journal_valid(const group_switch_journal_t *record);
bool group_switch_journal_bound(const group_switch_journal_t *record,
                                 const char *server_url, const char *device_id);
bool group_switch_terminal_matches(const group_switch_journal_t *record,
                                    const group_switch_terminal_t *terminal);
bool group_switch_state_confirms(const group_switch_terminal_t *terminal,
                                  const char *current_group_id, uint64_t current_version);

#ifndef GROUP_CONTROLLER_HOST_TEST
#include "esp_err.h"
/* Worker-only, no Token/CA/voice content. NOT_FOUND means no pending operation.
 * A different existing operation is never overwritten by save or erased by clear. */
esp_err_t group_switch_journal_load(group_switch_journal_t *record);
esp_err_t group_switch_journal_save(const group_switch_journal_t *record);
esp_err_t group_switch_journal_clear(const char *expected_operation_id);
#endif

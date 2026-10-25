#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define GROUP_RECOMMENDATION_MAX 3
#define GROUP_RECOMMENDATION_NEED_BYTES 2048

typedef enum {
    GROUP_RECOMMENDATION_START, GROUP_RECOMMENDATION_CANCEL, GROUP_RECOMMENDATION_CONFIRM,
} group_recommendation_action_t;
typedef enum {
    GROUP_RECOMMENDATION_IDLE, GROUP_RECOMMENDATION_PREPARING,
    GROUP_RECOMMENDATION_LISTENING, GROUP_RECOMMENDATION_CLEANUP,
    GROUP_RECOMMENDATION_REQUESTING, GROUP_RECOMMENDATION_CHOOSING,
    GROUP_RECOMMENDATION_CONFIRMING, GROUP_RECOMMENDATION_RECOVERING,
    GROUP_RECOMMENDATION_FAILED, GROUP_RECOMMENDATION_EXPIRED,
} group_recommendation_phase_t;
typedef struct {
    char name[129];
    char purpose[1025];
    char reason[641];
} group_recommendation_item_t;
typedef struct {
    group_recommendation_phase_t phase;
    uint32_t revision;
    unsigned count;
    bool can_start, can_cancel, can_confirm;
    uint32_t expires_remaining_ms;
    char detail[241];
} group_recommendation_view_t;
/* GUI-lock-safe intent admission only: no allocation, display, I/O or wait.
 * CONFIRM carries the exact published revision and selected index. */
typedef esp_err_t (*group_recommendation_action_cb_t)(group_recommendation_action_t action,
                                                     unsigned index, uint32_t revision, void *context);

/* Worker-owned, PSRAM-allocated result. Never place this bounded but large
 * structure on an internal task stack. UI sees only the display items above. */
typedef struct {
    char recommendation_id[81], request_id[81], server_epoch[81], candidate_digest[65];
    char expires_at[48];
    uint64_t current_version;
    int64_t expires_at_ms;
    unsigned count;
    char group_ids[GROUP_RECOMMENDATION_MAX][81];
    group_recommendation_item_t items[GROUP_RECOMMENDATION_MAX];
} group_recommendation_t;

bool group_recommendation_need_valid(const char *text);
struct cJSON;
bool group_recommendation_parse(const struct cJSON *response, const char *request_id,
                                const char *epoch, uint64_t version, group_recommendation_t *result);

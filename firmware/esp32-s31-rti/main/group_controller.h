#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "board_audio.h"
#include "esp_err.h"
#include "group_recommendation.h"

esp_err_t group_controller_init(const board_audio_t *audio);

/* Explicit init/config-write refresh only: may read NVS and validate CA, outside
 * the controller guard. Does not replace an active worker identity. STATUS uses
 * the resulting RAM snapshot, which survives STOP. */
void group_controller_refresh_config_status(void);

/* UI-safe intent updates: no HTTP, display calls, allocation, or blocking wait.
 * Release/stop synchronously close the peer admission gate under the same guard
 * used to validate a grant. Lifecycle work runs on the persistent worker. */
void group_controller_action(bool stop, void *context);
void group_controller_ptt(bool pressed, void *context);
esp_err_t group_controller_switch_group(const char *target_group_id, uint32_t view_generation, void *context);
void group_controller_print_status(void);
void group_controller_print_groups(void);

/* These intents synchronously cancel PTT; the worker performs all network I/O. */
esp_err_t group_controller_join_group(const char *code, void *context);
esp_err_t group_controller_voice_action(bool cancel, void *context);
esp_err_t group_controller_recommendation_action(group_recommendation_action_t action,
                                                 unsigned index, uint32_t revision, void *context);

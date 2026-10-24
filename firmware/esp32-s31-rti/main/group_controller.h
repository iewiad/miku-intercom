#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "board_audio.h"
#include "esp_err.h"

esp_err_t group_controller_init(const board_audio_t *audio);

/* UI-safe intent updates: no HTTP, display calls, allocation, or blocking wait.
 * Release/stop synchronously close the peer admission gate under the same guard
 * used to validate a grant. Lifecycle work runs on the persistent worker. */
void group_controller_action(bool stop, void *context);
void group_controller_ptt(bool pressed, void *context);
esp_err_t group_controller_switch_group(const char *target_group_id, uint32_t view_generation, void *context);
void group_controller_print_status(void);
void group_controller_print_groups(void);

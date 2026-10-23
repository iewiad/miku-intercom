#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    BOARD_CONVERSATION_IDLE = 0,
    BOARD_CONVERSATION_STARTING,
    BOARD_CONVERSATION_ACTIVE,
    BOARD_CONVERSATION_STOPPING,
    BOARD_CONVERSATION_ERROR,
} board_conversation_state_t;

typedef enum {
    BOARD_TRANSCRIPT_USER = 0,
    BOARD_TRANSCRIPT_ASSISTANT,
} board_transcript_role_t;

typedef void (*board_conversation_action_cb_t)(bool stop, void *context);
typedef void (*board_volume_action_cb_t)(uint8_t volume, bool persist, void *context);
typedef esp_err_t (*board_wifi_connected_cb_t)(void *context);

esp_err_t board_display_init(void);
esp_err_t board_display_show_wifi_setup(board_wifi_connected_cb_t callback, void *context);
esp_err_t board_display_show_wifi_connected(void);
esp_err_t board_display_show_conversation(board_conversation_action_cb_t callback,
                                          board_volume_action_cb_t volume_callback,
                                          uint8_t initial_volume,
                                          void *context);
esp_err_t board_display_set_conversation_state(board_conversation_state_t state,
                                               const char *detail,
                                               bool session_active);
esp_err_t board_display_set_transcript(board_transcript_role_t role, const char *text, bool streaming);
esp_err_t board_display_commit_transcript_turn(void);
esp_err_t board_display_reset_transcript(void);
bool board_display_is_chinese(void);

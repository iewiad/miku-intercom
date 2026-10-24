#pragma once

#include <stdbool.h>
#include <stddef.h>
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
typedef void (*board_ptt_action_cb_t)(bool pressed, void *context);
/* A successful callback only queues the switch. It must not perform I/O,
 * allocate, or call display functions while the LVGL lock is held. */
typedef esp_err_t (*board_group_switch_action_cb_t)(const char *target_group_id,
                                                    uint32_t view_generation,
                                                    void *context);
#define BOARD_GROUP_CHOICES_MAX 128

typedef struct {
    char id[81];
    char name[129];
} board_group_item_t;
typedef void (*board_volume_action_cb_t)(uint8_t volume, bool persist, void *context);
typedef esp_err_t (*board_wifi_connected_cb_t)(void *context);
/* Called with the display lock held: copy the bounded input and reserve the
 * command synchronously, then queue work. No I/O or display calls here. */
typedef esp_err_t (*board_group_join_action_cb_t)(const char *code, void *context);
typedef esp_err_t (*board_group_voice_action_cb_t)(bool cancel, void *context);

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

/** Group intercom uses independent connect and hold-to-talk controls. */
esp_err_t board_display_show_group(board_conversation_action_cb_t startstop,
                                   board_ptt_action_cb_t ptt,
                                   board_group_switch_action_cb_t switch_group,
                                   board_volume_action_cb_t volume,
                                   uint8_t initial_volume,
                                   void *context);
esp_err_t board_display_set_group_state(const char *group, const char *detail,
                                        bool connected, bool speaking);

/** Publish controller intent separately from transport readiness (worker only). */
esp_err_t board_display_set_group_active(bool active);

/** Worker-only publication. Strings/items are deep-copied into explicit PSRAM.
 * Same-generation refreshes preserve the candidate until confirmation. Publish
 * a new generation at a switch terminal result, including rejection, so a fast
 * transaction cannot leave the UI waiting for an unobserved pending frame. */
esp_err_t board_display_set_group_choices(const board_group_item_t *items,
                                          size_t count,
                                          const char *current_group_id,
                                          uint32_t view_generation,
                                          bool known,
                                          bool can_select,
                                          bool switching);

/** Register optional join-code and explicit voice-switch commands (worker only).
 * Join codes contain exactly eight characters from
 * ABCDEFGHJKLMNPQRSTUVWXYZ23456789. The UI never retains a submitted code. */
esp_err_t board_display_set_group_commands(board_group_join_action_cb_t join,
                                            board_group_voice_action_cb_t voice,
                                            void *context);

/** Authoritative command state publication (worker only). Increment revision
 * synchronously when accepting a callback and on every phase/terminal change.
 * Publish the resulting busy reservation before processing another command.
 * A fresh terminal revision MUST be published even if the busy phase completed
 * before the UI observed it. Repeated/superseded revisions cannot acknowledge
 * a newer UI request. can_cancel applies only before the switch is submitted;
 * after submission, keep voice_active true and can_cancel false until recovery.
 * Publication only renders state: it must not call controller action/PTT
 * callbacks or invalidate a connection ticket. Accepted commands and UI modal
 * opening already cancel PTT synchronously. detail is copied by LVGL; do not
 * include join codes or credentials. */
esp_err_t board_display_set_group_command_state(uint32_t revision,
                                                bool joining,
                                                bool voice_active,
                                                bool can_cancel,
                                                const char *detail);

#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Pure intent state. The controller owns the lock; no I/O or allocations here. */
typedef enum { GROUP_COMMAND_NONE, GROUP_COMMAND_JOIN, GROUP_COMMAND_VOICE, GROUP_COMMAND_RECOMMEND } group_command_kind_t;
typedef enum { GROUP_COMMAND_IDLE, GROUP_COMMAND_QUEUED, GROUP_COMMAND_STARTING,
    GROUP_COMMAND_LISTENING, GROUP_COMMAND_RESOLVING, GROUP_COMMAND_CLEANUP,
    GROUP_COMMAND_RECOVERING, GROUP_COMMAND_REQUESTING, GROUP_COMMAND_CHOOSING,
    GROUP_COMMAND_CONFIRMING } group_command_phase_t;
typedef struct {
    group_command_kind_t kind;
    group_command_phase_t phase;
    uint32_t revision;
    uint32_t ticket;
    uint32_t origin_revision;
    bool cancelled;
    bool failed;
    bool submitted;
    char code[9];
    char transcript[513];
    /* RECOMMEND-only worker-provided PSRAM before RTA starts. Cancellation
     * marks intent; only the lifecycle owner erases this request buffer. */
    char *need;
    size_t need_capacity;
} group_command_state_t;
bool group_command_code(const char *input, char output[9]);
bool group_command_need_valid(const char *text);
bool group_command_begin(group_command_state_t *s, group_command_kind_t kind, const char *code);
bool group_command_cancel(group_command_state_t *s);
bool group_command_ready(group_command_state_t *s, uint32_t ticket);
bool group_command_final(group_command_state_t *s, uint32_t ticket, const char *text);
bool group_command_error(group_command_state_t *s, uint32_t ticket);
void group_command_phase(group_command_state_t *s, group_command_phase_t phase);
void group_command_finish(group_command_state_t *s);

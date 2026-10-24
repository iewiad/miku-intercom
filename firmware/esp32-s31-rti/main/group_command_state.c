#include "group_command_state.h"
#include <stddef.h>
#include <string.h>

static void advance(uint32_t *v) { if (++*v == 0) ++*v; }
bool group_command_code(const char *input, char output[9])
{
    if (!input || strlen(input) != 8) return false;
    char code[9] = {0};
    for (size_t i = 0; i < 8; ++i) {
        unsigned char c = (unsigned char)input[i];
        if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
        if (!strchr("ABCDEFGHJKLMNPQRSTUVWXYZ23456789", c)) return false;
        code[i] = (char)c;
    }
    memcpy(output, code, sizeof(code));
    return true;
}
bool group_command_begin(group_command_state_t *s, group_command_kind_t kind, const char *code)
{
    if (!s || s->kind != GROUP_COMMAND_NONE || (kind != GROUP_COMMAND_JOIN && kind != GROUP_COMMAND_VOICE)) return false;
    char normalized[9] = {0};
    if (kind == GROUP_COMMAND_JOIN && !group_command_code(code, normalized)) return false;
    const uint32_t revision = s->revision, ticket = s->ticket;
    memset(s, 0, sizeof(*s));
    s->revision = revision; advance(&s->revision);
    s->ticket = ticket; advance(&s->ticket);
    s->kind = kind; s->phase = GROUP_COMMAND_QUEUED;
    memcpy(s->code, normalized, sizeof(normalized));
    return true;
}
bool group_command_cancel(group_command_state_t *s)
{
    if (!s || s->kind != GROUP_COMMAND_VOICE || s->submitted || s->cancelled || s->phase == GROUP_COMMAND_RECOVERING) return false;
    if (!s->cancelled) { s->cancelled = true; advance(&s->revision); }
    memset(s->transcript, 0, sizeof(s->transcript));
    return true;
}
void group_command_phase(group_command_state_t *s, group_command_phase_t phase)
{
    if (s->phase != phase) { s->phase = phase; advance(&s->revision); }
}
bool group_command_ready(group_command_state_t *s, uint32_t ticket)
{
    if (s->kind != GROUP_COMMAND_VOICE || s->ticket != ticket || s->cancelled || s->phase != GROUP_COMMAND_STARTING) return false;
    group_command_phase(s, GROUP_COMMAND_LISTENING);
    return true;
}
bool group_command_final(group_command_state_t *s, uint32_t ticket, const char *text)
{
    if (s->kind != GROUP_COMMAND_VOICE || s->ticket != ticket || s->cancelled || s->phase != GROUP_COMMAND_LISTENING || !text) return false;
    const size_t length = strlen(text);
    if (!length || length >= sizeof(s->transcript)) return group_command_error(s, ticket);
    memcpy(s->transcript, text, length + 1);
    group_command_phase(s, GROUP_COMMAND_RESOLVING);
    return true;
}
bool group_command_error(group_command_state_t *s, uint32_t ticket)
{
    if (s->kind != GROUP_COMMAND_VOICE || s->ticket != ticket || s->phase == GROUP_COMMAND_RECOVERING) return false;
    s->failed = true;
    memset(s->transcript, 0, sizeof(s->transcript));
    group_command_phase(s, GROUP_COMMAND_CLEANUP);
    return true;
}
void group_command_finish(group_command_state_t *s)
{
    const uint32_t revision = s->revision, ticket = s->ticket;
    memset(s, 0, sizeof(*s));
    s->revision = revision; advance(&s->revision);
    s->ticket = ticket; advance(&s->ticket);
}

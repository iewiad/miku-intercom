#include "group_command_state.h"
#include <stddef.h>
#include <string.h>

static void advance(uint32_t *v) { if (++*v == 0) ++*v; }
bool group_command_need_valid(const char *text)
{
    if (!text) return false;
    unsigned count = 0;
    size_t i = 0;
    while (text[i]) {
        if (i >= 2048) return false;
        const unsigned char lead = (unsigned char)text[i++];
        unsigned remaining = 0;
        uint32_t value = lead, minimum = 0;
        if (lead >= 0xc2 && lead <= 0xdf) { remaining = 1; value = lead & 31; minimum = 0x80; }
        else if (lead >= 0xe0 && lead <= 0xef) { remaining = 2; value = lead & 15; minimum = 0x800; }
        else if (lead >= 0xf0 && lead <= 0xf4) { remaining = 3; value = lead & 7; minimum = 0x10000; }
        else if (lead >= 0x80) return false;
        while (remaining--) {
            if (i >= 2048 || !text[i]) return false;
            const unsigned char next = (unsigned char)text[i++];
            if ((next & 0xc0) != 0x80) return false;
            value = (value << 6) | (next & 63);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff) ||
            value < 0x20 || (value >= 0x7f && value <= 0x9f) || ++count > 512) return false;
    }
    return count != 0;
}
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
    if (!s || s->kind != GROUP_COMMAND_NONE || (kind != GROUP_COMMAND_JOIN && kind != GROUP_COMMAND_VOICE && kind != GROUP_COMMAND_RECOMMEND)) return false;
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
    if (!s || (s->kind != GROUP_COMMAND_VOICE && s->kind != GROUP_COMMAND_RECOMMEND) || s->submitted || s->cancelled || s->phase == GROUP_COMMAND_RECOVERING) return false;
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
    if ((s->kind != GROUP_COMMAND_VOICE && s->kind != GROUP_COMMAND_RECOMMEND) || s->ticket != ticket || s->cancelled || s->phase != GROUP_COMMAND_STARTING) return false;
    group_command_phase(s, GROUP_COMMAND_LISTENING);
    return true;
}
bool group_command_final(group_command_state_t *s, uint32_t ticket, const char *text)
{
    if ((s->kind != GROUP_COMMAND_VOICE && s->kind != GROUP_COMMAND_RECOMMEND) || s->ticket != ticket || s->cancelled || s->phase != GROUP_COMMAND_LISTENING || !text) return false;
    const size_t length = strlen(text);
    if (!length || (s->kind == GROUP_COMMAND_VOICE && length >= sizeof(s->transcript)) ||
        (s->kind == GROUP_COMMAND_RECOMMEND && (!s->need || length >= s->need_capacity ||
                                               !group_command_need_valid(text)))) return group_command_error(s, ticket);
    if (s->kind == GROUP_COMMAND_RECOMMEND) memcpy(s->need, text, length + 1);
    else memcpy(s->transcript, text, length + 1);
    group_command_phase(s, GROUP_COMMAND_RESOLVING);
    return true;
}
bool group_command_error(group_command_state_t *s, uint32_t ticket)
{
    if ((s->kind != GROUP_COMMAND_VOICE && s->kind != GROUP_COMMAND_RECOMMEND) || s->ticket != ticket ||
        s->submitted || s->phase == GROUP_COMMAND_RECOVERING) return false;
    s->failed = true;
    memset(s->transcript, 0, sizeof(s->transcript));
    group_command_phase(s, GROUP_COMMAND_CLEANUP);
    return true;
}
void group_command_finish(group_command_state_t *s)
{
    const uint32_t revision = s->revision, ticket = s->ticket;
    if (s->need) memset(s->need, 0, s->need_capacity);
    memset(s, 0, sizeof(*s));
    s->revision = revision; advance(&s->revision);
    s->ticket = ticket; advance(&s->ticket);
}

#include "group_command_state.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    group_command_state_t s = {0}; char code[9];
    assert(group_command_code("abcd2345", code) && !strcmp(code,"ABCD2345"));
    const char *bad[] = {"", "ABCDEFG", "ABCDEFGHI", "ABCD0123", "ABCDI234", "ABCDO234", " ABC2345", "ABCD23\n5"};
    for (unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) assert(!group_command_code(bad[i],code));
    assert(!group_command_begin(&s,GROUP_COMMAND_JOIN,"BAD"));
    assert(group_command_begin(&s,GROUP_COMMAND_JOIN,"abcd2345"));
    assert(!strcmp(s.code,"ABCD2345") && s.revision && s.ticket);
    assert(!group_command_begin(&s,GROUP_COMMAND_VOICE,NULL));
    assert(!group_command_cancel(&s));
    group_command_finish(&s); assert(!s.code[0] && s.kind==GROUP_COMMAND_NONE);
    assert(group_command_begin(&s,GROUP_COMMAND_VOICE,NULL));
    uint32_t old=s.ticket, revision=s.revision;
    assert(!group_command_ready(&s,old));
    group_command_phase(&s,GROUP_COMMAND_STARTING);
    assert(!group_command_final(&s,old,"切到后勤组"));
    assert(!group_command_ready(&s,old-1));
    assert(group_command_ready(&s,old));
    assert(s.revision>revision);
    assert(group_command_final(&s,old,"切到后勤组"));
    assert(!group_command_final(&s,old,"切到其他组"));
    assert(group_command_cancel(&s));
    assert(!s.transcript[0] && !group_command_final(&s,old,"切到其他组"));
    group_command_finish(&s);
    assert(group_command_begin(&s,GROUP_COMMAND_VOICE,NULL));
    group_command_phase(&s,GROUP_COMMAND_STARTING);
    assert(!group_command_ready(&s,old));
    assert(group_command_ready(&s,s.ticket));
    char large[514]; memset(large,'x',sizeof(large));large[513]=0;
    assert(group_command_final(&s,s.ticket,large));
    assert(s.failed && s.phase==GROUP_COMMAND_CLEANUP && !s.transcript[0]);
    group_command_phase(&s,GROUP_COMMAND_RECOVERING);s.submitted=true;
    assert(!group_command_cancel(&s));
    assert(!group_command_error(&s,s.ticket));
    group_command_finish(&s);assert(s.kind==GROUP_COMMAND_NONE && !s.transcript[0]);
    s.revision=s.ticket=UINT32_MAX;
    assert(group_command_begin(&s,GROUP_COMMAND_VOICE,NULL));
    assert(s.revision==1 && s.ticket==1);
    group_command_finish(&s);
    char need[2049], unicode[2053];
    for (unsigned i=0;i<512;++i) memcpy(unicode+i*4,"😀",4);
    unicode[2048]=0;
    assert(group_command_need_valid(unicode));
    assert(group_command_begin(&s,GROUP_COMMAND_RECOMMEND,NULL));
    s.need=need;s.need_capacity=sizeof(need);
    group_command_phase(&s,GROUP_COMMAND_STARTING);assert(group_command_ready(&s,s.ticket));
    assert(group_command_final(&s,s.ticket,unicode)&&!s.failed&&strlen(need)==2048&&!s.transcript[0]);
    assert(group_command_cancel(&s)); // Worker still owns need until cleanup.
    assert(strlen(need)==2048);group_command_finish(&s);assert(!need[0]);
    unicode[2048]='x';unicode[2049]=0;assert(!group_command_need_valid(unicode));
    assert(!group_command_need_valid("bad\ncontrol")&&!group_command_need_valid("\xc0\xaf"));
    puts("group command state: passed (code, busy, first final, stale, cancel, overflow, terminal, revision wrap)");
}

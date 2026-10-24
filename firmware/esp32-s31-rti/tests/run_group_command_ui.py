#!/usr/bin/env python3
"""Run the real display command guards and revision ACKs with LVGL/worker doubles.

Covers validation, empty-membership join, local pending and stale publication,
fast terminal ACK, voice cancel boundary, one-way busy/recovery publications,
and stopped/unknown state. Does not
render pixels, execute LVGL keyboard internals, or validate physical touch.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
from run_http_client_reuse import extract_function

PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_TIMEOUT 2
#define LV_EVENT_CLICKED 3
#define LV_STATE_DISABLED 1
typedef struct { int code; } lv_event_t;
static int lv_event_get_code(lv_event_t *e) { return e->code; }
#define strlcpy test_strlcpy
static size_t test_strlcpy(char *dst,const char *src,size_t cap) {
 size_t n=strlen(src); if(cap) { size_t c=n<cap-1?n:cap-1; memcpy(dst,src,c);dst[c]=0; } return n;
}
static int label_object, ptt_object, textarea_object, dropdown_object;
static void *group_command_detail=&label_object, *group_detail_label=&label_object;
static void *group_ptt_button=&ptt_object;
static void *group_join_textarea=&textarea_object, *group_dropdown=&dropdown_object;
static void *group_switch_button=&label_object, *group_switch_label=&label_object;
static void *group_join_button=&label_object, *group_voice_button=&label_object;
static void *group_status_label=&label_object, *group_ptt_label=&label_object;
static bool lv_dropdown_is_open(void *o) { (void)o;return false; }
static uint32_t lv_dropdown_get_selected(void *o) { (void)o;return 0; }
static void lv_obj_remove_state(void *o,int state) { (void)o;(void)state; }
static void lv_obj_add_state(void *o,int state) { (void)o;(void)state; }
static char typed[32], rendered[192];
static const char *lv_textarea_get_text(void *o) { (void)o; return typed; }
static void lv_label_set_text(void *o,const char *s) { (void)o;strlcpy(rendered,s,sizeof(rendered)); }
static void lv_dropdown_close(void *o) { (void)o; }
static bool lock_ok=true;
static bool bsp_display_lock(int ms) { (void)ms; return lock_ok; }
static void bsp_display_unlock(void) {}
static void group_render_command(void) {}
static void group_render_controls(void);
typedef enum { GROUP_MODAL_NONE,GROUP_MODAL_JOIN,GROUP_MODAL_VOICE } group_modal_t;
static group_modal_t group_modal;
static bool group_joining,group_voice_active,group_voice_can_cancel;
static bool group_command_pending,group_voice_submitted;
static bool group_choices_valid,group_stop_action,group_transport_ready;
static uint32_t group_command_revision,group_command_pending_revision;
static char group_command_detail_text[192];
static struct { bool known,can_select; size_t count; char current_id[81];
 struct {char id[81];} items[1]; } choices;
static typeof(choices) *group_choices=&choices;
static bool is_switching;
static int current_index;
static bool group_pressed,group_speaking,group_connected,group_candidate_valid;
static unsigned ptt_calls, connection_generation;
static void *group_context;
static void ptt_action(bool pressed,void *ctx) { (void)ctx;(void)pressed;++ptt_calls;++connection_generation; }
static void (*group_ptt_callback)(bool,void*)=ptt_action;
static esp_err_t (*group_switch_callback)(const char*,uint32_t,void*);
static bool group_is_switching(void) { return is_switching; }
static int group_current_index(void) { return current_index; }

static void group_close_command(void) { typed[0]=0;group_modal=GROUP_MODAL_NONE; }
static void *group_command_context;
static int join_calls,voice_calls,cancel_calls;
static char submitted[9];
static esp_err_t callback_result=ESP_OK;
static esp_err_t join_action(const char *code,void *ctx) {
 (void)ctx; ++join_calls;strlcpy(submitted,code,sizeof(submitted));return callback_result;
}
static esp_err_t voice_action(bool cancel,void *ctx) {
 (void)ctx; if(cancel) ++cancel_calls;else ++voice_calls;return callback_result;
}
static esp_err_t (*group_join_callback)(const char*,void*)=join_action;
static esp_err_t (*group_voice_callback)(bool,void*)=voice_action;
'''

SUFFIX = r'''
static void reset(void) {
 group_choices_valid=true;choices.known=true;choices.count=0;group_stop_action=true;
 group_transport_ready=false;current_index=-1;is_switching=false;
 group_pressed=group_speaking=group_connected=group_candidate_valid=false;
 ptt_calls=0;connection_generation=123;
 group_joining=group_voice_active=group_voice_can_cancel=false;
 group_command_pending=group_voice_submitted=false;group_command_revision=7;
 group_modal=GROUP_MODAL_JOIN;typed[0]=0;submitted[0]=0;
 callback_result=ESP_OK;join_calls=voice_calls=cancel_calls=0;
}
int main(void) {
 reset();
 // Joining a known empty membership does not require a ready media Peer.
 assert(group_command_ready());strlcpy(typed,"ABCDEFG2",sizeof(typed));
 group_join_submit();
 assert(join_calls==1 && strcmp(submitted,"ABCDEFG2")==0);
 assert(group_command_pending && group_command_pending_revision==7);
 assert(typed[0]==0 && group_modal==GROUP_MODAL_NONE);
 // An older idle snapshot cannot unlock the submitted intent.
 assert(board_display_set_group_command_state(7,false,false,false,"old idle")==ESP_OK);
 assert(group_command_pending && group_command_revision==7);
 // Even an unobserved, fast terminal update acknowledges the request.
 assert(board_display_set_group_command_state(9,false,false,false,"joined")==ESP_OK);
 assert(!group_command_pending && !group_joining && group_command_revision==9);
 assert(strcmp(group_command_detail_text,"joined")==0);
 assert(board_display_set_group_command_state(8,true,false,false,"stale busy")==ESP_OK);
 assert(!group_joining && group_command_revision==9);
 reset();strlcpy(typed,"ABCDEF01",sizeof(typed));group_join_submit();assert(join_calls==0);
 strlcpy(typed,"ABCDEFG",sizeof(typed));group_join_submit();assert(join_calls==0);
 choices.known=false;strlcpy(typed,"ABCDEFG2",sizeof(typed));group_join_submit();assert(join_calls==0);
 choices.known=true;group_stop_action=false;group_join_submit();assert(join_calls==0);
 reset();callback_result=ESP_ERR_INVALID_STATE;strlcpy(typed,"ABCDEFG2",sizeof(typed));
 group_join_submit();assert(join_calls==1 && !group_command_pending && group_modal==GROUP_MODAL_JOIN);
 // Voice requires a ready current group. A repeated click is suppressed.
 reset();group_modal=GROUP_MODAL_VOICE;lv_event_t click={.code=LV_EVENT_CLICKED};
 group_voice_submit_event_cb(&click);assert(voice_calls==0);
 group_transport_ready=true;current_index=0;group_voice_submit_event_cb(&click);
 assert(voice_calls==1 && group_voice_submitted && group_command_pending && ptt_calls==1);
 group_voice_submit_event_cb(&click);assert(voice_calls==1);
 board_display_set_group_command_state(8,false,true,true,"say group name");
 assert(group_voice_active && group_voice_can_cancel && !group_command_pending);
 group_command_cancel_event_cb(&click);
 assert(cancel_calls==1 && group_command_pending && group_command_pending_revision==8);
 board_display_set_group_command_state(8,false,true,true,"stale listening");
 assert(group_command_pending);
 board_display_set_group_command_state(9,false,false,false,"cancelled and restored");
 assert(!group_command_pending && !group_voice_active);
 // Once switch submission begins, cancel is unavailable until recovery.
 board_display_set_group_command_state(10,false,true,false,"confirming switch");
 group_command_cancel_event_cb(&click);assert(cancel_calls==1 && group_modal==GROUP_MODAL_VOICE);
 board_display_set_group_command_state(11,false,true,false,"restoring");
 group_command_cancel_event_cb(&click);assert(cancel_calls==1);
 board_display_set_group_command_state(12,false,false,false,"ready");
 group_command_cancel_event_cb(&click);assert(group_modal==GROUP_MODAL_NONE);
 // The production publisher AND control renderer must not feed busy/recovery
 // frames back into PTT and invalidate the worker's connection generation.
 reset();group_pressed=true;group_speaking=true;
 const unsigned connect_ticket=connection_generation;
 board_display_set_group_command_state(8,true,false,false,"joining");
 assert(!group_pressed&&!group_speaking&&ptt_calls==0&&connection_generation==connect_ticket);
 for(unsigned i=0;i<5;++i) {
  board_display_set_group_command_state(8,true,false,false,"same busy snapshot");
  assert(ptt_calls==0&&connection_generation==connect_ticket);
 }
 board_display_set_group_command_state(9,false,true,true,"listening");
 assert(ptt_calls==0&&connection_generation==connect_ticket);
 board_display_set_group_command_state(10,false,true,false,"recovering");
 for(unsigned i=0;i<5;++i)board_display_set_group_command_state(10,false,true,false,"recovering refresh");
 board_display_set_group_command_state(11,false,true,false,"fresh recovering frame");
 assert(ptt_calls==0&&connection_generation==connect_ticket);
 board_display_set_group_command_state(12,false,false,false,"recovered");
 assert(ptt_calls==0&&connection_generation==connect_ticket);
 // Revision wrap remains ordered within the protocol's bounded interval.
 group_command_revision=UINT32_MAX;group_command_pending=true;group_command_pending_revision=UINT32_MAX;
 board_display_set_group_command_state(0,false,false,false,"wrapped terminal");
 assert(group_command_revision==0 && !group_command_pending);
 lock_ok=false;assert(board_display_set_group_command_state(1,false,false,false,"")==ESP_ERR_TIMEOUT);
 puts("display commands: passed (code guard, empty join, fast/stale ACK, duplicate voice, cancel boundary, one-way recovery publication, wrap)");
 return 0;
}
'''

def main():
    root = Path(__file__).resolve().parents[1]
    source = (root / 'main/board_display.c').read_text()
    names = ['group_command_busy', 'group_cancel_ptt', 'group_force_release',
             'group_can_choose', 'group_render_controls', 'group_command_ready', 'group_join_submit',
             'group_voice_submit_event_cb', 'group_command_cancel_event_cb',
             'board_display_set_group_command_state']
    # Order the authoritative publisher before the test main; the callback
    # bodies only need declarations supplied by the small SDK doubles above.
    definitions = {n: source[source.index('static void group_render_controls(void)\n{'):] if n == 'group_render_controls' else source for n in names}
    body = PREFIX + '\n' + '\n'.join(extract_function(definitions[n], n) for n in names) + SUFFIX
    compiler = shutil.which('cc')
    if not compiler:
        raise SystemExit('cc is required')
    with tempfile.TemporaryDirectory(prefix='intercom-display-') as folder:
        src = Path(folder) / 'display_commands.c'
        exe = Path(folder) / 'display_commands'
        src.write_text(body)
        subprocess.run([compiler, '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)

if __name__ == '__main__':
    main()

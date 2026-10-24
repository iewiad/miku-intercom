#!/usr/bin/env python3
"""Run production legacy auto-join and manual queue guards with real cJSON.

No network/NVS/UI/RTOS is used. This verifies an obsolete USB code cannot keep
consuming the bad-code allowance; a known empty catalog remains manually joinable.
"""
from pathlib import Path
import argparse
import shutil
import subprocess
import tempfile
from run_http_client_reuse import extract_function

PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "group_command_state.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NO_MEM 3
#define HTTP_METHOD_GET 1
#define HTTP_METHOD_POST 2
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
static int guard;
static int64_t esp_timer_get_time(void) {return 1000;}
static struct {char group_code[17];} config;
static char group_id[81];
static bool legacy_join_attempted,catalog_known;
static struct {bool desired,link_lost,switch_pending,command_busy;} intent;
static group_command_state_t command;
static unsigned reads,posts,manual_notifications;
static bool fail_auto_join=true;
static void cancel_locked(void) {}
static void notify_worker(void) {++manual_notifications;}
static void mbedtls_platform_zeroize(void *p,size_t n) {volatile unsigned char *c=p;while(n--)*c++=0;}
static esp_err_t apply_state(const cJSON *state,int64_t started,int64_t ended) {
 (void)started;(void)ended;assert(cJSON_IsObject(state));catalog_known=true;return ESP_OK;
}
static esp_err_t reject_state(esp_err_t err) {catalog_known=false;return err;}
static esp_err_t request(const char *path,int method,const cJSON *body,cJSON **out) {
 if(!strcmp(path,"/state")) {assert(method==HTTP_METHOD_GET&&!body);++reads;*out=cJSON_CreateObject();return ESP_OK;}
 assert(!strcmp(path,"/memberships")&&method==HTTP_METHOD_POST);++posts;
 assert(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(body,"code")));
 if(fail_auto_join)return ESP_ERR_INVALID_ARG;
 *out=cJSON_Parse("{\"state\":{}}");return ESP_OK;
}
'''
SUFFIX = r'''
static void reset(void) {
 memset(&config,0,sizeof(config));memset(&intent,0,sizeof(intent));memset(&command,0,sizeof(command));
 group_id[0]=0;legacy_join_attempted=false;catalog_known=false;intent.desired=true;
 reads=posts=manual_notifications=0;fail_auto_join=true;
}
int main(void) {
 reset();assert(refresh_state(true)==ESP_OK);assert(reads==1&&posts==0&&!legacy_join_attempted&&catalog_known);
 reset();memcpy(config.group_code,"ABCDEFG2",9);
 assert(refresh_state(true)!=ESP_OK);assert(reads==1&&posts==1&&legacy_join_attempted&&catalog_known);
 for(unsigned i=0;i<10;++i)assert(refresh_state(true)==ESP_OK);
 assert(posts==1&&reads==11&&catalog_known);
 // Correct keypad input is a separate production command and remains eligible.
 assert(group_controller_join_group("BCDEFGH3",NULL)==ESP_OK);
 assert(command.kind==GROUP_COMMAND_JOIN&&intent.command_busy&&manual_notifications==1);
 assert(strcmp(command.code,"BCDEFGH3")==0&&legacy_join_attempted);
 reset();memcpy(config.group_code,"ABCDEFG2",9);fail_auto_join=false;
 assert(refresh_state(true)==ESP_OK);assert(refresh_state(true)==ESP_OK);assert(posts==1);
 reset();memcpy(config.group_code,"ABCDEFG2",9);
 assert(refresh_state(false)==ESP_OK);assert(!legacy_join_attempted&&posts==0);
 // A loaded new configuration session resets this worker-owned allowance.
 legacy_join_attempted=false;assert(refresh_state(true)!=ESP_OK);assert(posts==1);
 puts("legacy auto-join: passed (optional code, one attempt, no retry loop, manual correction remains queued)");
 return 0;
}
'''

def main():
    project=Path(__file__).resolve().parents[1]
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cjson-dir',type=Path,default=project/'managed_components/espressif__cjson/cJSON')
    args=parser.parse_args()
    if not all((args.cjson_dir/p).is_file() for p in ('cJSON.c','cJSON.h')):
        parser.error('pass --cjson-dir pointing to existing cJSON.c/.h')
    compiler=shutil.which('cc')
    if not compiler:parser.error('cc is required')
    production=(project/'main/group_controller.c').read_text()
    funcs='\n\n'.join(extract_function(production,n) for n in ('refresh_state','group_controller_join_group'))
    with tempfile.TemporaryDirectory(prefix='intercom-legacy-join-') as tmp:
        tmp=Path(tmp);src=tmp/'legacy_join.c';exe=tmp/'legacy_join'
        src.write_text(PREFIX+funcs+SUFFIX)
        subprocess.run([compiler,'-std=gnu11','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations',
                        '-fsanitize=address,undefined','-I',str(project/'main'),'-I',str(args.cjson_dir),
                        str(src),str(project/'main/group_command_state.c'),str(args.cjson_dir/'cJSON.c'),'-lm','-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)

if __name__=='__main__':main()

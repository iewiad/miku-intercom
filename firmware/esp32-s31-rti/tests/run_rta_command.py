#!/usr/bin/env python3
"""Production RTA command parser/state and session payload regression with real cJSON.

No network, RTOS scheduling, actual microphone or provider acknowledgement is
simulated. Uses local cJSON only; --sanitize enables ASan/UBSan.
"""
from pathlib import Path
import argparse
import re
import shutil
import subprocess
import tempfile
from run_http_client_reuse import C_TOKENS

def extract_function(source, name):
    declaration=re.search(rf"^(?:static\s+)?(?:const\s+)?\w+\s+\*?\s*{re.escape(name)}\s*\(",source,re.MULTILINE)
    if declaration is None: raise RuntimeError(f"Missing production function: {name}")
    depth=0
    for token in C_TOKENS.finditer(source,declaration.end()):
        if token.group()=="{": depth+=1
        elif token.group()=="}":
            depth-=1
            if depth==0: return source[declaration.start():token.end()]
    raise RuntimeError(f"Incomplete production function: {name}")

PREFIX = r'''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "rta_command_events.h"
#define CONFIG_MIKU_RTI_ENABLE_WEB_SEARCH 1
#define CONFIG_MIKU_RTI_DEVICE_ID "test-device"
#define CONFIG_MIKU_RTI_MODEL_PROFILE "test-model"
#define CONFIG_MIKU_RTI_ROLE_PROFILE "test-role"
#define CONFIG_MIKU_RTI_REGION "test-region"
#define CONFIG_MIKU_RTI_WEB_SEARCH_PROVIDER "test-provider"
#define APP_DISPLAY_NAME "test-app"
typedef struct { char app_id[32]; } rti_credentials_t;
static const char *rti_session_get_voice_profile(void) { return "test-voice"; }
static bool board_display_is_chinese(void) { return true; }
static int alloc_index, fail_at, live_allocations;
static void *test_malloc(size_t size) {
    ++alloc_index;
    if (fail_at && alloc_index == fail_at) return NULL;
    void *p = malloc(size);
    if (p) ++live_allocations;
    return p;
}
static void test_free(void *p) { if (p) { --live_allocations; free(p); } }
static rta_command_input_t parsed(const char *json) {
    rta_command_input_t input;
    rta_command_parse_event((const uint8_t *)json, strlen(json), &input);
    return input;
}
static void assert_payload(const char *payload, bool command) {
    cJSON *root=cJSON_Parse(payload); assert(root);
    const cJSON *s=cJSON_GetObjectItemCaseSensitive(root,"session"); assert(s);
    const cJSON *turn=cJSON_GetObjectItemCaseSensitive(s,"turn_detection"); assert(turn);
    const cJSON *response=cJSON_GetObjectItemCaseSensitive(turn,"create_response");
    assert(command ? cJSON_IsFalse(response) : cJSON_IsTrue(response));
    const cJSON *asr=cJSON_GetObjectItemCaseSensitive(s,"input_audio_transcription");
    const cJSON *model=cJSON_GetObjectItemCaseSensitive(asr,"model");
    assert(cJSON_IsString(model) && strcmp(model->valuestring,"qwen3-asr-flash-realtime")==0);
    assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(s,"modalities"))==2);
    const cJSON *search=cJSON_GetObjectItemCaseSensitive(s,"enable_search");
    assert(command ? search==NULL : cJSON_IsTrue(search));
    assert(!strstr(payload,"response.create"));
    cJSON_Delete(root);
}
'''

SUFFIX = r'''
int main(void) {
    webrtc_command_event_t event;
    rta_command_state_t state;
    rta_command_input_t ack=parsed("{\"type\":\"session.updated\"}");
    rta_command_input_t final=parsed("{\"type\":\"conversation.item.input_audio_transcription.completed\",\"transcript\":\"切到维修组\"}");
    assert(final.type==RTA_COMMAND_INPUT_FINAL);
    assert(!strcmp(final.event_type,"conversation.item.input_audio_transcription.completed"));
    assert(!strcmp(final.parse_reason,"accepted"));
    rta_command_input_t diagnostic=parsed("{\"type\":\"do-not-log-this-provider-controlled-value\"}");
    assert(!strcmp(diagnostic.event_type,"other") && !strcmp(diagnostic.parse_reason,"ignored"));
    diagnostic=parsed("{\"type\":\"rtid.error\",\"code\":\"invalid_session_update\",\"message\":\"provider text must never be logged\"}");
    assert(diagnostic.type==RTA_COMMAND_INPUT_ERROR && !strcmp(diagnostic.event_type,"rtid.error"));
    assert(!strcmp(diagnostic.parse_reason,"invalid_session_update"));
    diagnostic=parsed("{\"type\":\"rtid.error\",\"code\":\"do-not-log-provider-controlled-value\"}");
    assert(diagnostic.type==RTA_COMMAND_INPUT_ERROR && !strcmp(diagnostic.parse_reason,"provider_error"));
    diagnostic=parsed("{\"type\":\"session.closed\"}");
    assert(diagnostic.type==RTA_COMMAND_INPUT_ERROR);
    diagnostic=parsed("not-json");
    assert(!strcmp(diagnostic.event_type,"unavailable") && !strcmp(diagnostic.parse_reason,"malformed_json"));
    rta_command_begin(&state,7);
    assert(!rta_command_media_allowed(&state));
    assert(!rta_command_accept(&state,7,&final,&event));
    assert(!rta_command_accept(&state,7,&ack,&event));
    assert(rta_command_audio_available(&state,&event));
    assert(event.type==WEBRTC_COMMAND_READY && event.generation==7);
    assert(rta_command_media_allowed(&state));
    assert(!rta_command_accept(&state,6,&final,&event));
    assert(rta_command_accept(&state,7,&final,&event));
    assert(event.type==WEBRTC_COMMAND_FINAL && event.generation==7 && !strcmp(event.text,"切到维修组"));
    assert(!rta_command_media_allowed(&state));
    assert(!rta_command_accept(&state,7,&final,&event));
    assert(!rta_command_accept(&state,7,&ack,&event));
    rta_command_begin(&state,8);
    assert(!rta_command_audio_available(&state,&event));
    assert(rta_command_accept(&state,8,&ack,&event));
    assert(event.type==WEBRTC_COMMAND_READY);
    rta_command_cancel(&state);
    assert(!rta_command_media_allowed(&state));
    assert(!rta_command_accept(&state,8,&final,&event));
    assert(!rta_command_audio_available(&state,&event));
    rta_command_begin(&state,9);
    assert(!rta_command_accept(&state,8,&ack,&event));
    assert(!rta_command_accept(&state,9,&ack,&event));
    assert(rta_command_audio_available(&state,&event));
    rta_command_input_t error=parsed("{\"type\":\"conversation.item.input_audio_transcription.failed\"}");
    assert(rta_command_accept(&state,9,&error,&event));
    assert(event.type==WEBRTC_COMMAND_ERROR && event.error==ESP_FAIL);
    assert(!rta_command_media_allowed(&state));
    assert(parsed("{\"type\":\"response.text.done\",\"text\":\"维修组\"}").type==RTA_COMMAND_INPUT_IGNORE);
    assert(parsed("{\"type\":\"response.audio_transcript.done\",\"transcript\":\"维修组\"}").type==RTA_COMMAND_INPUT_IGNORE);
    assert(parsed("{\"type\":\"conversation.item.input_audio_transcription.delta\",\"delta\":\"维修组\"}").type==RTA_COMMAND_INPUT_IGNORE);
    assert(parsed("{\"type\":\"conversation.item.input_audio_transcription.completed\",\"transcript\":\" \\t\"}").type==RTA_COMMAND_INPUT_ERROR);
    assert(parsed("{\"type\":\"conversation.item.input_audio_transcription.completed\",\"transcript\":12}").type==RTA_COMMAND_INPUT_ERROR);
    assert(parsed("{\"type\":\"conversation.item.input_audio_transcription.completed\",\"transcript\":\"a\\u0000b\"}").type==RTA_COMMAND_INPUT_ERROR);
    assert(parsed("{\"type\":\"session.updated\"}garbage").type==RTA_COMMAND_INPUT_IGNORE);
    char text[520], json[800];
    memset(text,'a',512); text[512]=0;
    snprintf(json,sizeof(json),"{\"type\":\"conversation.item.input_audio_transcription.completed\",\"transcript\":\"%s\"}",text);
    assert(parsed(json).type==RTA_COMMAND_INPUT_FINAL);
    text[512]='b';text[513]=0;
    snprintf(json,sizeof(json),"{\"type\":\"conversation.item.input_audio_transcription.completed\",\"transcript\":\"%s\"}",text);
    assert(parsed(json).type==RTA_COMMAND_INPUT_ERROR);
    text[0]=(char)0xc0;text[1]=(char)0xaf;text[2]=0;
    snprintf(json,sizeof(json),"{\"type\":\"conversation.item.input_audio_transcription.completed\",\"transcript\":\"%s\"}",text);
    assert(parsed(json).type==RTA_COMMAND_INPUT_ERROR);
    // Exact 512-byte mixed UTF-8 final accepted, boundary+1 rejected unchanged.
    for (size_t i=0;i<170;++i) memcpy(text+i*3,"组",3);
    text[510]='a';text[511]='b';text[512]=0;
    snprintf(json,sizeof(json),"{\"type\":\"conversation.item.input_audio_transcription.completed\",\"transcript\":\"%s\"}",text);
    assert(parsed(json).type==RTA_COMMAND_INPUT_FINAL);
    text[512]='c';text[513]=0;
    snprintf(json,sizeof(json),"{\"type\":\"conversation.item.input_audio_transcription.completed\",\"transcript\":\"%s\"}",text);
    assert(parsed(json).type==RTA_COMMAND_INPUT_ERROR);
    char *payload=build_session_update_payload(true);assert(payload);assert_payload(payload,true);free(payload);
    payload=build_session_update_payload(false);assert(payload);assert_payload(payload,false);free(payload);
    rti_credentials_t credentials={.app_id="test-app-id"};
    cJSON *create=build_create_request(&credentials,true);assert(create);
    assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(create,"enable_search")));
    assert(!cJSON_GetObjectItemCaseSensitive(create,"realtime_tools"));
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(create,"scenario")->valuestring,"esp32_s31_group_command"));
    assert(!cJSON_GetObjectItemCaseSensitive(create,"welcome"));
    cJSON_Delete(create);
    create=build_create_request(&credentials,false);assert(create);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(create,"enable_search")));
    assert(cJSON_GetObjectItemCaseSensitive(create,"realtime_tools"));cJSON_Delete(create);
    // Fail each cJSON allocation in turn: never emit an incomplete payload,
    // never leak intermediate objects, and retain create_response=false.
    cJSON_Hooks hooks={.malloc_fn=test_malloc,.free_fn=test_free};cJSON_InitHooks(&hooks);
    for (int i=1;i<=100;++i) {
        alloc_index=0;fail_at=i;
        payload=build_session_update_payload(true);
        fail_at=0;
        if (payload) { assert_payload(payload,true);test_free(payload); }
        assert(live_allocations==0);
        alloc_index=0;fail_at=i;
        create=build_create_request(&credentials,true);
        fail_at=0;
        if (create) {
            assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(create,"enable_search")));
            cJSON_Delete(create);
        }
        assert(live_allocations==0);
    }
    cJSON_InitHooks(NULL);
    puts("RTA command: final-only UTF-8 bounds, ready/terminal/cancel/generation, command+legacy payloads and allocation failures passed");
    return 0;
}
'''


def main():
    project=Path(__file__).resolve().parents[1]
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cjson-dir',type=Path,default=project/'managed_components/espressif__cjson/cJSON')
    parser.add_argument('--sanitize',action='store_true')
    args=parser.parse_args()
    cc=shutil.which('cc')
    if not cc: parser.error('system cc is required')
    if not all((args.cjson_dir/name).is_file() for name in ('cJSON.c','cJSON.h')):
        parser.error('pass --cjson-dir pointing to existing cJSON source')
    source=(project/'main/webrtc_peer.c').read_text()
    sessions=(project/'main/rti_session.c').read_text()
    code=PREFIX+'\n'+extract_function(source,'build_session_update_payload')+'\n'+extract_function(sessions,'build_create_request')+'\n'+SUFFIX
    flags=['-std=c11','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror']
    if args.sanitize: flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
    with tempfile.TemporaryDirectory(prefix='intercom-rta-command-') as directory:
        temp=Path(directory)
        (temp/'esp_err.h').write_text('#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n#define ESP_ERR_NO_MEM 0x101\n#define ESP_ERR_INVALID_RESPONSE 0x108\n')
        (temp/'fixture.c').write_text(code)
        subprocess.run([cc,*flags,'-I',str(temp),'-I',str(project/'main'),'-I',str(args.cjson_dir),str(temp/'fixture.c'),str(project/'main/rta_command_events.c'),str(args.cjson_dir/'cJSON.c'),'-o',str(temp/'fixture')],check=True)
        subprocess.run([str(temp/'fixture')],check=True)

if __name__=='__main__': main()

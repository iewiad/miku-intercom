#!/usr/bin/env python3
"""Real create-response parser and cleanup ownership; no SDK, network or keys."""
from pathlib import Path
import argparse
import shutil
import subprocess
import tempfile
from run_rta_command import extract_function
from run_rta_stop import PREFIX

EXTRA = r'''
#define RTI_APP_STATE_SESSION_CREATING 1
#define RTI_APP_STATE_ERROR 2
#define RTI_APP_STATE_READY 3
static int legacy_stops,app_state;
static void rti_app_set_state(int state) { app_state=state; }
static cJSON *build_create_request(const rti_credentials_t *c,bool command) {
    (void)c;(void)command;return cJSON_CreateObject();
}
static size_t test_strlcpy(char *out,const char *in,size_t capacity) {
    size_t n=strlen(in);if(capacity){size_t copied=n<capacity-1?n:capacity-1;memcpy(out,in,copied);out[copied]=0;}return n;
}
#define strlcpy test_strlcpy
esp_err_t rti_session_stop(const rti_session_t *session,const char *reason) {
    assert(session->session_id[0]);assert(!strcmp(reason,"invalid_client_endpoint"));++legacy_stops;return ESP_FAIL;
}
'''
PERFORM = r'''
static esp_err_t esp_http_client_perform(mock_client_t *c) {
    ++performed;assert(c->authorization && c->method==HTTP_METHOD_POST && c->post_count==1);
    assert(!strcmp(c->config.url,"https://synthetic.invalid/rtic/v1/realtime/sessions"));
    const char *body=responses[c->index].body;
    if(body){esp_http_client_event_t event={.event_id=HTTP_EVENT_ON_DATA,.user_data=c->config.user_data,.data=(void *)body,.data_len=(int)strlen(body)};(void)c->config.event_handler(&event);}
    return responses[c->index].error;
}
'''
SUFFIX = r'''
static void reset(const char *body) {
    memset(responses,0,sizeof(responses));responses[0]=(scripted_response_t){.status=200,.body=body};
    created=performed=cleaned=credentials_loaded=legacy_body_logs=legacy_stops=app_state=0;
    expect_command=false;
}
static void assert_only_cleanup_id(const rti_session_t *session) {
    rti_session_t expected={.session_id="session_test-1"};assert(!memcmp(session,&expected,sizeof(expected)));
}
int main(void) {
    rti_session_t session;
    const char *missing_token="{\"session_id\":\"session_test-1\",\"trace_id\":\"trace\",\"client_endpoint\":{\"url\":\"/rtc\",\"expires_at_ms\":123}}";
    reset(missing_token);
    assert(rti_session_create_command(&session)==ESP_ERR_INVALID_RESPONSE);
    assert_only_cleanup_id(&session);assert(legacy_stops==0 && app_state==RTI_APP_STATE_ERROR);
    reset(missing_token);
    assert(rti_session_create(&session)==ESP_ERR_INVALID_RESPONSE);
    rti_session_t empty={0};assert(!memcmp(&session,&empty,sizeof(session)));assert(legacy_stops==1);
    reset("{\"session_id\":\"../unsafe\",\"trace_id\":\"trace\"}");
    assert(rti_session_create_command(&session)==ESP_ERR_INVALID_RESPONSE);
    assert(!memcmp(&session,&empty,sizeof(session)) && legacy_stops==0);
    reset("{\"session_id\":\"session_test-1\",\"trace_id\":42}");
    assert(rti_session_create_command(&session)==ESP_ERR_INVALID_RESPONSE);
    assert_only_cleanup_id(&session);assert(legacy_stops==0);
    reset("{\"trace_id\":\"trace\"}");
    assert(rti_session_create_command(&session)==ESP_ERR_INVALID_RESPONSE);
    assert(!memcmp(&session,&empty,sizeof(session)) && legacy_stops==0);
    reset(missing_token);responses[0].status=500;
    assert(rti_session_create_command(&session)==ESP_FAIL);
    assert(!memcmp(&session,&empty,sizeof(session)) && legacy_stops==0);
    reset(missing_token);responses[0].status=0;responses[0].error=ESP_FAIL;
    assert(rti_session_create_command(&session)==ESP_FAIL);
    assert(!memcmp(&session,&empty,sizeof(session)) && legacy_stops==0);
    reset("{\"session_id\":\"session_test-1\",\"trace_id\":\"trace\",\"client_endpoint\":{\"url\":\"/rtc\",\"access_token\":\"synthetic-endpoint-token\",\"expires_at_ms\":123}}");
    assert(rti_session_create_command(&session)==ESP_OK);
    assert(!strcmp(session.session_id,"session_test-1") && !strcmp(session.rtc_url,"https://synthetic.invalid/rtc"));
    assert(legacy_stops==0 && app_state==RTI_APP_STATE_READY);
    puts("RTA create cleanup: known safe ID retained on command parse failure, endpoint secrets cleared, legacy fallback unchanged passed");
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
    if not cc or not (args.cjson_dir/'cJSON.c').is_file(): parser.error('system cc and existing --cjson-dir are required')
    production=(project/'main/rti_session.c').read_text()
    prefix=PREFIX.replace(extract_function(PREFIX,'esp_http_client_perform'),PERFORM)
    names=('http_event_handler','copy_json_string','parse_create_response','create_session','rti_session_create','rti_session_create_command')
    code=prefix+'\n'+EXTRA+'\n'+'\n\n'.join(extract_function(production,name) for name in names)+'\n'+SUFFIX
    flags=['-std=c11','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror']
    if args.sanitize: flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
    with tempfile.TemporaryDirectory(prefix='intercom-rta-create-') as directory:
        temp=Path(directory)
        (temp/'esp_err.h').write_text('#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n#define ESP_ERR_NO_MEM 0x101\n#define ESP_ERR_INVALID_ARG 0x102\n#define ESP_ERR_INVALID_STATE 0x103\n#define ESP_ERR_INVALID_RESPONSE 0x108\n')
        (temp/'fixture.c').write_text(code)
        subprocess.run([cc,*flags,'-I',str(temp),'-I',str(project/'main'),'-I',str(args.cjson_dir),str(temp/'fixture.c'),str(args.cjson_dir/'cJSON.c'),'-o',str(temp/'fixture')],check=True)
        subprocess.run([str(temp/'fixture')],check=True)

if __name__=='__main__':main()

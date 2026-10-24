#!/usr/bin/env python3
"""Production command stop/GET terminal proof with real cJSON and bounded HTTP double.

No credentials, network, provider or hardware are accessed. Ordinary RTA stop
acknowledgement semantics are verified separately. --sanitize enables ASan/UBSan.
"""
from pathlib import Path
import argparse
import shutil
import subprocess
import tempfile
from run_rta_command import extract_function

PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "rti_session.h"
#define CONFIG_MIKU_RTI_DEVICE_API_BASE_URL "https://synthetic.invalid"
#define RTI_HTTP_RESPONSE_MAX (8 * 1024)
#define RTI_HTTP_TIMEOUT_MS 20000
#define HTTP_EVENT_ON_DATA 1
#define HTTP_METHOD_POST 1
#define HTTP_METHOD_GET 2
#define ESP_LOGE(...) quiet_log(__VA_ARGS__)
#define ESP_LOGW(...) quiet_log(__VA_ARGS__)
#define ESP_LOGI(...) quiet_log(__VA_ARGS__)
static const char *TAG="test";
static void quiet_log(const char *tag,const char *format,...) { (void)tag;(void)format; }
static const char *esp_err_to_name(esp_err_t e) { (void)e;return "synthetic_error"; }
typedef struct { char api_token[64]; } rti_credentials_t;
static int credentials_loaded, legacy_body_logs;
static esp_err_t rti_credentials_load(rti_credentials_t *c) {
    ++credentials_loaded; strcpy(c->api_token,"synthetic-test-key");return ESP_OK;
}
static void mbedtls_platform_zeroize(void *p,size_t n) {
    volatile unsigned char *bytes=p;while(n--) *bytes++=0;
}
static void log_api_error(const char *op,int status,const char *json) {
    (void)op;(void)status;(void)json;++legacy_body_logs;
}
typedef struct { char *data;size_t length,capacity;bool overflowed; } http_response_buffer_t;
typedef struct { int event_id;void *user_data;void *data;int data_len; } esp_http_client_event_t;
static esp_err_t esp_crt_bundle_attach(void *unused) { (void)unused;return ESP_OK; }
typedef struct {
    const char *url;
    esp_err_t (*event_handler)(esp_http_client_event_t *);
    void *user_data;
    esp_err_t (*crt_bundle_attach)(void *);
    int timeout_ms,buffer_size;
    bool disable_auto_redirect;
} esp_http_client_config_t;
typedef struct { int status;const char *body;esp_err_t error;bool header_failure; } scripted_response_t;
typedef struct {
    esp_http_client_config_t config;
    int index,method,post_count;
    bool authorization;
    char body[256];
} mock_client_t;
typedef mock_client_t *esp_http_client_handle_t;
static scripted_response_t responses[2];
static int created,performed,cleaned;
static bool expect_command;
static esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg) {
    assert(created<2);
    assert(cfg->timeout_ms==20000 && cfg->buffer_size==2048);
    assert(cfg->crt_bundle_attach==esp_crt_bundle_attach);
    assert(cfg->disable_auto_redirect==expect_command);
    mock_client_t *client=calloc(1,sizeof(*client));assert(client);
    client->config=*cfg;client->index=created++;
    return client;
}
static esp_err_t esp_http_client_set_method(mock_client_t *c,int method) { c->method=method;return ESP_OK; }
static esp_err_t esp_http_client_set_header(mock_client_t *c,const char *key,const char *value) {
    if (responses[c->index].header_failure) return ESP_FAIL;
    if (!strcmp(key,"Authorization")) {
        assert(!strcmp(value,"Bearer synthetic-test-key"));c->authorization=true;
    } else if (!strcmp(key,"Accept") || !strcmp(key,"Content-Type")) assert(!strcmp(value,"application/json"));
    else assert(false);
    return ESP_OK;
}
static esp_err_t esp_http_client_set_post_field(mock_client_t *c,const char *body,int length) {
    assert(length>0 && length<(int)sizeof(c->body));
    memcpy(c->body,body,(size_t)length);c->body[length]=0;++c->post_count;return ESP_OK;
}
static esp_err_t esp_http_client_perform(mock_client_t *c) {
    ++performed;assert(c->authorization);
    if (c->index==0) {
        assert(c->method==HTTP_METHOD_POST && c->post_count==1);
        assert(!strcmp(c->config.url,"https://synthetic.invalid/rtic/v1/realtime/sessions/session_test-1/stop"));
        cJSON *body=cJSON_Parse(c->body);assert(body);
        const cJSON *reason=cJSON_GetObjectItemCaseSensitive(body,"reason");
        assert(cJSON_IsString(reason) && !strcmp(reason->valuestring,"group_command_finished"));cJSON_Delete(body);
    } else {
        assert(expect_command && c->method==HTTP_METHOD_GET && c->post_count==0);
        assert(!strcmp(c->config.url,"https://synthetic.invalid/rtic/v1/realtime/sessions/session_test-1"));
    }
    const char *body=responses[c->index].body;
    if (body) {
        esp_http_client_event_t event={.event_id=HTTP_EVENT_ON_DATA,.user_data=c->config.user_data,.data=(void *)body,.data_len=(int)strlen(body)};
        (void)c->config.event_handler(&event);
    }
    return responses[c->index].error;
}
static int esp_http_client_get_status_code(mock_client_t *c) { return responses[c->index].status; }
static void esp_http_client_cleanup(mock_client_t *c) { ++cleaned;free(c); }
'''

SUFFIX = r'''
static const char *terminal="{\"session\":{\"session_id\":\"session_test-1\",\"state\":\"ended\"}}";
static const char *connected="{\"session\":{\"session_id\":\"session_test-1\",\"state\":\"provider_connected\"}}";
static void reset(bool command,const char *body) {
    memset(responses,0,sizeof(responses));
    responses[0]=(scripted_response_t){.status=202,.body=connected};
    responses[1]=(scripted_response_t){.status=200,.body=body};
    created=performed=cleaned=credentials_loaded=legacy_body_logs=0;expect_command=command;
}
static esp_err_t parse(const char *body) { return parse_command_terminal(body,strlen(body),"session_test-1"); }
int main(void) {
    rti_session_t session={.session_id="session_test-1"};
    assert(parse(terminal)==ESP_OK);
    assert(parse("{\"session\":{\"session_id\":\"session_test-1\",\"state\":\"failed\"}}")==ESP_OK);
    assert(parse("{\"session\":{\"session_id\":\"session_test-1\",\"state\":\"expired\"}}")==ESP_OK);
    assert(parse(connected)==ESP_ERR_INVALID_STATE);
    assert(parse("{\"session\":{\"session_id\":\"session_test-1\",\"state\":\"stopping\"}}")==ESP_ERR_INVALID_STATE);
    assert(parse("{\"session_id\":\"session_test-1\",\"state\":\"ended\"}")==ESP_ERR_INVALID_RESPONSE);
    assert(parse("{\"session\":{\"session_id\":\"other\",\"state\":\"ended\"}}")==ESP_ERR_INVALID_RESPONSE);
    assert(parse("{\"session\":{\"session_id\":\"session_test-1\",\"state\":\"ended\",\"state\":\"provider_connected\"}}")==ESP_ERR_INVALID_RESPONSE);
    assert(parse("{\"session\":{\"session_id\":\"session_test-1\",\"session_id\":\"other\",\"state\":\"ended\"}}")==ESP_ERR_INVALID_RESPONSE);
    assert(parse("{\"session\":{},\"session\":{\"session_id\":\"session_test-1\",\"state\":\"ended\"}}")==ESP_ERR_INVALID_RESPONSE);
    assert(parse("{\"session\":{\"session_id\":\"session_test-1\",\"state\":\"ended\\u0000junk\"}}")==ESP_ERR_INVALID_RESPONSE);
    assert(parse("{\"session\":{\"session_id\":\"session_test-1\",\"state\":\"ended\"}}garbage")==ESP_ERR_INVALID_RESPONSE);
    assert(parse("{}")==ESP_ERR_INVALID_RESPONSE);
    char rawnul[]={ '{','}',0,'g','a','r','b','a','g','e',0 };
    assert(parse_command_terminal(rawnul,sizeof(rawnul)-1,"session_test-1")==ESP_ERR_INVALID_RESPONSE);
    reset(true,connected);
    assert(rti_session_stop_command(&session,"group_command_finished")==ESP_ERR_INVALID_STATE);
    assert(created==2 && performed==2 && cleaned==2 && credentials_loaded==2 && legacy_body_logs==0);
    reset(true,terminal);
    assert(rti_session_stop_command(&session,"group_command_finished")==ESP_OK);
    assert(created==2 && performed==2 && cleaned==2 && legacy_body_logs==0);
    reset(true,terminal);responses[1].status=302;
    assert(rti_session_stop_command(&session,"group_command_finished")==ESP_FAIL);
    assert(created==2 && performed==2 && cleaned==2 && legacy_body_logs==0);
    reset(true,terminal);responses[1].error=ESP_FAIL;
    assert(rti_session_stop_command(&session,"group_command_finished")==ESP_FAIL);
    reset(true,terminal);responses[0].error=ESP_FAIL;
    assert(rti_session_stop_command(&session,"group_command_finished")==ESP_FAIL);
    assert(created==1 && performed==1 && cleaned==1);
    reset(true,terminal);responses[0].status=302;
    assert(rti_session_stop_command(&session,"group_command_finished")==ESP_FAIL);
    assert(created==1 && performed==1 && legacy_body_logs==0);
    reset(true,terminal);responses[1].header_failure=true;
    assert(rti_session_stop_command(&session,"group_command_finished")==ESP_FAIL);
    assert(created==2 && performed==1 && cleaned==2);
    char oversized[RTI_HTTP_RESPONSE_MAX+1];memset(oversized,'x',sizeof(oversized)-1);oversized[sizeof(oversized)-1]=0;
    reset(true,oversized);
    assert(rti_session_stop_command(&session,"group_command_finished")==ESP_ERR_NO_MEM);
    reset(false,terminal);
    assert(rti_session_stop(&session,"group_command_finished")==ESP_OK);
    assert(created==1 && performed==1 && cleaned==1 && credentials_loaded==1);
    // Preserve even the legacy ordinary path's 2xx acknowledgement behavior.
    reset(false,terminal);responses[0].error=ESP_FAIL;
    assert(rti_session_stop(&session,"group_command_finished")==ESP_OK);
    assert(created==1 && performed==1);
    reset(true,terminal);
    assert(rti_session_stop_command(NULL,"group_command_finished")==ESP_ERR_INVALID_ARG);
    const char *unsafe[]={"","../escape","id?query","id/other","id\\other","id%2fother","id#fragment","id with-space"};
    for (size_t i=0;i<sizeof(unsafe)/sizeof(*unsafe);++i) {
        memset(&session,0,sizeof(session));strcpy(session.session_id,unsafe[i]);
        assert(rti_session_stop_command(&session,"group_command_finished")==ESP_ERR_INVALID_ARG);
    }
    memset(session.session_id,'x',sizeof(session.session_id));
    assert(rti_session_stop_command(&session,"group_command_finished")==ESP_ERR_INVALID_ARG);
    assert(created==0 && credentials_loaded==0);
    puts("RTA stop command: strict wrapped terminal proof, POST/GET/auth/redirect/bounds, transport failures and legacy acknowledgement compatibility passed");
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
        parser.error('pass --cjson-dir pointing to an existing cJSON source directory')
    source=(project/'main/rti_session.c').read_text()
    functions='\n\n'.join(extract_function(source,name) for name in ('http_event_handler','stop_session','rti_session_stop','command_session_id_valid','unique_json_field','parse_command_terminal','query_command_terminal','rti_session_stop_command'))
    code=PREFIX+'\n'+functions+'\n'+SUFFIX
    flags=['-std=c11','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror']
    if args.sanitize: flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
    with tempfile.TemporaryDirectory(prefix='intercom-rta-stop-') as directory:
        temp=Path(directory)
        (temp/'esp_err.h').write_text('#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n#define ESP_ERR_NO_MEM 0x101\n#define ESP_ERR_INVALID_ARG 0x102\n#define ESP_ERR_INVALID_STATE 0x103\n#define ESP_ERR_INVALID_RESPONSE 0x108\n')
        (temp/'fixture.c').write_text(code)
        subprocess.run([cc,*flags,'-I',str(temp),'-I',str(project/'main'),'-I',str(args.cjson_dir),str(temp/'fixture.c'),str(args.cjson_dir/'cJSON.c'),'-o',str(temp/'fixture')],check=True)
        subprocess.run([str(temp/'fixture')],check=True)

if __name__=='__main__': main()

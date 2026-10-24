#!/usr/bin/env python3
"""Real command bootstrap admission/send counters with a serial SDK double.

No RTOS scheduling, mic driver, network or concurrency is simulated. Confirms
only fixed Opus silence is admitted before READY and no new admission follows
cancel/final/error; hardware/RTOS timing remains a separate validation.
"""
from pathlib import Path
import argparse
import re
import shutil
import subprocess
import tempfile
from run_rta_command import extract_function

PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "rta_command_events.h"
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define ESP_PEER_ERR_NONE 0
/* @production-stats */
static int group_state_lock;
static rta_command_state_t command_state;
static webrtc_command_stats_t command_stats;
static struct { bool command_mode,running,audio_running;void *peer; } rtc;
typedef struct { uint32_t pts;uint8_t *data;int size; } esp_peer_audio_frame_t;
static unsigned packets;
static int send_result;
static uint32_t last_pts;
static int esp_peer_send_audio(void *peer,const esp_peer_audio_frame_t *frame) {
    static const uint8_t generated[]={0xf8,0xff,0xfe};
    assert(peer==rtc.peer && frame->size==3 && !memcmp(frame->data,generated,3));
    ++packets;last_pts=frame->pts;return send_result;
}
'''
SUFFIX = r'''
int main(void) {
    rtc.command_mode=rtc.running=rtc.audio_running=true;rtc.peer=&rtc;
    rta_command_begin(&command_state,1);
    uint32_t pts=1000;
    command_send_bootstrap(&pts);
    assert(!packets && !command_stats.bootstrap_admitted && !command_stats.microphone_reads);
    assert(!command_admit_microphone_read());
    webrtc_command_event_t event;
    assert(!rta_command_audio_available(&command_state,&event));
    for(int i=0;i<5;++i) command_send_bootstrap(&pts);
    assert(packets==5 && last_pts==1080 && pts==1100);
    assert(command_stats.bootstrap_admitted==5 && command_stats.bootstrap_sent==5);
    assert(command_stats.microphone_reads==0 && !command_admit_microphone_read());
    send_result=ESP_FAIL;command_send_bootstrap(&pts);send_result=ESP_PEER_ERR_NONE;
    assert(command_stats.bootstrap_admitted==6 && command_stats.bootstrap_sent==5);
    webrtc_peer_command_cancel();
    command_send_bootstrap(&pts);
    assert(packets==6 && command_stats.bootstrap_admitted==6 && !command_admit_microphone_read());
    rta_command_begin(&command_state,2);
    assert(!rta_command_audio_available(&command_state,&event));
    const rta_command_input_t ack={.type=RTA_COMMAND_INPUT_UPDATED};
    assert(rta_command_accept(&command_state,2,&ack,&event));
    assert(event.type==WEBRTC_COMMAND_READY);
    command_send_bootstrap(&pts);
    assert(packets==6 && command_stats.bootstrap_admitted==6);
    assert(command_admit_microphone_read() && command_stats.microphone_reads==1);
    const rta_command_input_t final={.type=RTA_COMMAND_INPUT_FINAL,.text="fixture"};
    assert(rta_command_accept(&command_state,2,&final,&event));
    command_send_bootstrap(&pts);
    assert(packets==6 && !command_admit_microphone_read());
    rta_command_begin(&command_state,3);
    assert(!rta_command_audio_available(&command_state,&event));
    const rta_command_input_t error={.type=RTA_COMMAND_INPUT_ERROR,.error=ESP_FAIL};
    assert(rta_command_accept(&command_state,3,&error,&event));
    command_send_bootstrap(&pts);
    assert(packets==6 && !command_admit_microphone_read());
    rta_command_begin(&command_state,4);
    assert(!rta_command_audio_available(&command_state,&event));
    rtc.command_mode=false;command_send_bootstrap(&pts);assert(packets==6);
    rtc.command_mode=true;rtc.audio_running=false;command_send_bootstrap(&pts);assert(packets==6);
    rtc.audio_running=true;rtc.running=false;command_send_bootstrap(&pts);assert(packets==6);
    rtc.running=true;rtc.peer=NULL;command_send_bootstrap(&pts);assert(packets==6);
    webrtc_command_stats_t stats;webrtc_peer_command_get_stats(&stats);
    assert(stats.bootstrap_admitted==6 && stats.bootstrap_sent==5 && stats.microphone_reads==1);
    puts("Command bootstrap: fixed silence only after audio init, no mic before READY, cancel/final/error closure, separate counters and legacy isolation passed");
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
    if not cc or not (args.cjson_dir/'cJSON.c').is_file():parser.error('system cc and existing --cjson-dir required')
    source=(project/'main/webrtc_peer.c').read_text()
    header=(project/'main/webrtc_peer.h').read_text()
    stats=re.search(r'typedef struct \{[^}]+\} webrtc_command_stats_t;',header)
    if not stats:raise RuntimeError('production command stats missing')
    names=('webrtc_peer_command_get_stats','command_admit_microphone_read','command_admit_bootstrap','command_send_bootstrap','webrtc_peer_command_cancel')
    code=PREFIX.replace('/* @production-stats */',stats.group())+'\n'+'\n\n'.join(extract_function(source,name) for name in names)+'\n'+SUFFIX
    flags=['-std=c11','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror']
    if args.sanitize:flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
    with tempfile.TemporaryDirectory(prefix='intercom-bootstrap-') as directory:
        temp=Path(directory)
        (temp/'esp_err.h').write_text('#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n#define ESP_ERR_NO_MEM 0x101\n#define ESP_ERR_INVALID_RESPONSE 0x108\n')
        (temp/'fixture.c').write_text(code)
        subprocess.run([cc,*flags,'-I',str(temp),'-I',str(project/'main'),'-I',str(args.cjson_dir),str(temp/'fixture.c'),str(project/'main/rta_command_events.c'),str(args.cjson_dir/'cJSON.c'),'-o',str(temp/'fixture')],check=True)
        subprocess.run([str(temp/'fixture')],check=True)

if __name__=='__main__':main()

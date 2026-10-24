#!/usr/bin/env python3
"""Compile real peer admission functions with a queue/clock SDK double.

Checks irreversible suspension, late grants, receive-admission predicates,
packet ingress and legacy-path isolation. Does not execute playback_task,
its queue-out/decode/I2S call sites, real concurrency, or hardware.
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
#include <inttypes.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define ESP_LOGW(...) quiet_log(__VA_ARGS__)
static const char *TAG="test";
static void quiet_log(const char *tag,const char *format,...) { (void)tag; (void)format; }
#define RTC_OPUS_PACKET_MAX 1500
#define RTC_AUDIO_PACKET_GAP_WARN_US 80000
#define pdTRUE 1
static int group_state_lock;
static bool group_mode_active, group_transport_connected, group_audio_ready;
static bool group_media_suspended, group_send_enabled;
static int64_t group_send_expires_at_us, now_us = 1000;
static uint32_t group_send_epoch;
static struct { uint64_t received_packets; } group_stats;
typedef struct { uint8_t *data; int size; uint32_t pts; } esp_peer_audio_frame_t;
typedef struct { size_t size; int64_t received_us; uint32_t pts; uint8_t data[1500]; } opus_packet_t;
static struct {
 bool audio_running, group_mode, command_mode, has_last_audio_pts;
 void *playback_queue;
 uint32_t last_audio_pts, playback_input_gaps, playback_overflows;
 int64_t last_audio_packet_us;
} rtc;
static int queued;
static opus_packet_t queued_packet;
static unsigned uxQueueMessagesWaiting(void *q) { (void)q; return (unsigned)queued; }
static int64_t esp_timer_get_time(void) { return now_us; }
static int xQueueSend(void *q, const void *item, int ticks) {
 (void)q; (void)ticks;
 if (queued) return 0;
 memcpy(&queued_packet,item,sizeof(queued_packet)); queued=1; return pdTRUE;
}
static int xQueueReceive(void *q, void *item, int ticks) {
 (void)q; (void)ticks;
 if (!queued) return 0;
 memcpy(item,&queued_packet,sizeof(queued_packet)); queued=0; return pdTRUE;
}
'''
SUFFIX = r'''
int main(void) {
 uint32_t old_epoch, new_epoch;
 uint8_t opus[] = {0xf8,0xff,0xfe};
 esp_peer_audio_frame_t frame = {.data=opus,.size=3,.pts=960};
 rtc.audio_running=true; rtc.group_mode=true; rtc.playback_queue=&queued;
 group_mode_active=true; group_transport_connected=true; group_audio_ready=true;
 assert(webrtc_peer_group_connected());
 assert(group_receiving_allowed());
 // Receive without a floor is allowed, but uplink needs a current grant.
 assert(!group_sending_allowed(&old_epoch));
 assert(peer_audio_data_handler(&frame,NULL)==0);
 assert(queued==1 && group_stats.received_packets==1);
 webrtc_peer_group_set_sending(true,now_us+1000);
 assert(group_sending_allowed(&old_epoch));
 webrtc_peer_group_suspend_media();
 assert(!webrtc_peer_group_connected());
 assert(!group_sending_allowed(&new_epoch) && new_epoch!=old_epoch);
 assert(!group_receiving_allowed());
 // A late successful floor ACK cannot reopen the old Peer.
 webrtc_peer_group_set_sending(true,now_us+5000);
 assert(!group_sending_allowed(NULL));
 // The receive predicate rejects an existing queued entry; later ingress
 // cannot replace it or grow statistics. This fixture does not execute
 // playback_task or assert its queue-out/I2S call-site wiring.
 frame.pts+=960;
 assert(peer_audio_data_handler(&frame,NULL)==0);
 assert(queued==1 && group_stats.received_packets==1);
 assert(!group_receiving_allowed());
 webrtc_peer_group_suspend_media();
 assert(!group_receiving_allowed());
 // After explicit replacement, a new transport can receive. A new floor is
 // still required; an expired one cannot authorize transmission.
 group_media_suspended=false; group_send_enabled=false;
 assert(group_receiving_allowed());
 now_us+=10000;
 webrtc_peer_group_set_sending(true,now_us-1);
 assert(!group_sending_allowed(NULL));
 webrtc_peer_group_set_sending(true,now_us+1000);
 assert(group_sending_allowed(NULL));
 now_us+=1001;
 assert(!group_sending_allowed(NULL));
 // Command sessions drop AI audio before any queue admission.
 rtc.command_mode=true; queued=0;
 assert(peer_audio_data_handler(&frame,NULL)==0 && queued==0);
 rtc.command_mode=false;
 // Legacy RTA receive bypasses group admission and remains functional.
 group_mode_active=false; rtc.group_mode=false; queued=0;
 group_media_suspended=true;
 webrtc_peer_group_suspend_media();
 assert(peer_audio_data_handler(&frame,NULL)==0 && queued==1);
 puts("group media admission: passed (suspend, late grant, receive predicate/late ingress, expiry, RTA isolation)");
 return 0;
}
'''

def main():
    cc = shutil.which('cc')
    if not cc:
        raise SystemExit('The system C compiler cc is required.')
    source = (Path(__file__).resolve().parents[1]/'main/webrtc_peer.c').read_text()
    names = ('webrtc_peer_group_set_sending','group_sending_allowed',
             'webrtc_peer_group_connected','webrtc_peer_group_suspend_media',
             'group_receiving_allowed','peer_audio_data_handler')
    code = PREFIX+'\n\n'+'\n\n'.join(extract_function(source,n) for n in names)+'\n\n'+SUFFIX
    with tempfile.TemporaryDirectory(prefix='intercom-media-gate-') as directory:
        generated=Path(directory)/'gate.c';generated.write_text(code)
        executable=Path(directory)/'gate'
        subprocess.run([cc,'-std=c11','-Wall','-Wextra','-Werror',str(generated),'-o',str(executable)],check=True)
        subprocess.run([str(executable)],check=True)

if __name__=='__main__':
    main()

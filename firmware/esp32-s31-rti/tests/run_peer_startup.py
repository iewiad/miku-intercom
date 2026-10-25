#!/usr/bin/env python3
"""Execute real peer startup/stop functions with a threaded SDK double.

The allocator models the observed contiguous internal-stack failure. Threads
exercise the startup gate, partial failures and retained cleanup; they do not
simulate the ESP scheduler, transport, codecs or actual hardware heap.
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
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef int esp_err_t;
typedef unsigned EventBits_t;
typedef uint64_t TickType_t;
typedef struct { int unused; } esp_peer_cfg_t;
typedef struct event { pthread_mutex_t mutex; pthread_cond_t changed; unsigned bits; } *EventGroupHandle_t;
typedef struct task { pthread_t thread; void (*fn)(void *); void *argument; atomic_bool retired; } *TaskHandle_t;
static _Thread_local TaskHandle_t self;
static pthread_mutex_t group_state_lock = PTHREAD_MUTEX_INITIALIZER;
#define portENTER_CRITICAL(p) pthread_mutex_lock(p)
#define portEXIT_CRITICAL(p) pthread_mutex_unlock(p)
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_TIMEOUT 0x107
#define pdFALSE 0
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT64_MAX
#define pdMS_TO_TICKS(ms) ((ms) / 10)
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_SPIRAM 4
#define ESP_PEER_ERR_NONE 0
#define RTC_PIPELINE_STATS_INTERVAL_US 5000000
/* @production-defines */
static const char *TAG = "test";
static char last_wait[512], last_cleanup[512], last_peer_data[512];
static void quiet_log(const char *tag, const char *format, ...) {
 (void)tag;char line[512];va_list args;va_start(args,format);vsnprintf(line,sizeof(line),format,args);va_end(args);
 if(strstr(line,"RTC wait "))memcpy(last_wait,line,sizeof(last_wait));
 if(strstr(line,"RTC stop_wait ")&&strstr(line,"stage=cleanup "))memcpy(last_cleanup,line,sizeof(last_cleanup));
 if(strstr(line,"RTC stop_wait ")&&strstr(line,"stage=peer_data "))memcpy(last_peer_data,line,sizeof(last_peer_data));
}
#define ESP_LOGI(...) quiet_log(__VA_ARGS__)
#define ESP_LOGW(...) quiet_log(__VA_ARGS__)
#define ESP_LOGE(...) quiet_log(__VA_ARGS__)
static struct {
 void *peer,*lock,*data_queue,*playback_queue,*opus_encoder,*opus_decoder,*aec,*reference_stream;
 EventGroupHandle_t events;
 TaskHandle_t loop_task,data_task,mic_task,playback_task;
 atomic_bool running;
 bool audio_running,command_mode,group_mode;
 atomic_uint retired_workers;
 char *local_sdp;
 unsigned playback_input_gaps;
 int last_peer_state;
} rtc;
static void *persistent_aec;
static bool group_mode_active;
static uint32_t peer_cycle;
static TaskHandle_t tasks[4];
static unsigned task_count;
static bool fail_reserve,fail_open,fail_data;
static atomic_bool hold_loop;
static atomic_bool hold_retirement;
static bool fixture_command_active;
static atomic_uint loop_calls,open_calls,close_calls;
static unsigned largest_internal, reserved_stack;
static const char *esp_err_to_name(int err) { (void)err; return "fixture"; }
static int64_t esp_timer_get_time(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000; }
static TickType_t xTaskGetTickCount(void) { return (TickType_t)(esp_timer_get_time()/1000); }
static void vTaskDelay(TickType_t ticks) { struct timespec d={.tv_sec=0,.tv_nsec=(long)(ticks?ticks:1)*1000000};nanosleep(&d,NULL); }
static unsigned uxTaskGetStackHighWaterMark(void *task) { assert(!task);return 8192; }
static unsigned uxQueueMessagesWaiting(void *queue) { (void)queue;return 0; }
static EventBits_t xEventGroupSetBits(EventGroupHandle_t e,EventBits_t bits) {
 assert(e);pthread_mutex_lock(&e->mutex);e->bits|=bits;unsigned observed=e->bits;
 pthread_cond_broadcast(&e->changed);pthread_mutex_unlock(&e->mutex);
 if(bits==RTC_EVENT_LOOP_DONE)while(atomic_load(&hold_retirement))vTaskDelay(1);
 return observed;
}
static EventBits_t xEventGroupWaitBits(EventGroupHandle_t e,EventBits_t wanted,int clear,int all,TickType_t timeout) {
 assert(e);pthread_mutex_lock(&e->mutex);
 struct timespec deadline;clock_gettime(CLOCK_REALTIME,&deadline);
 if(timeout!=portMAX_DELAY){deadline.tv_sec+=(time_t)(timeout/1000);deadline.tv_nsec+=(long)(timeout%1000)*1000000;if(deadline.tv_nsec>=1000000000){++deadline.tv_sec;deadline.tv_nsec-=1000000000;}}
 while(all?(e->bits&wanted)!=wanted:!(e->bits&wanted)) {
  if(!timeout)break;
  int r=timeout==portMAX_DELAY?pthread_cond_wait(&e->changed,&e->mutex):pthread_cond_timedwait(&e->changed,&e->mutex,&deadline);
  if(r==ETIMEDOUT)break;assert(!r);
 }
 unsigned bits=e->bits;if(clear)e->bits&=~wanted;pthread_mutex_unlock(&e->mutex);return bits;
}
static void vEventGroupDelete(EventGroupHandle_t e) {
 assert((rtc.retired_workers&((rtc.loop_task?RTC_EVENT_LOOP_DONE:0)|(rtc.data_task?RTC_EVENT_DATA_DONE:0)))==((rtc.loop_task?RTC_EVENT_LOOP_DONE:0)|(rtc.data_task?RTC_EVENT_DATA_DONE:0)));
 pthread_cond_destroy(&e->changed);pthread_mutex_destroy(&e->mutex);free(e);
}
static void vSemaphoreDelete(void *p) { free(p); }
static void vQueueDeleteWithCaps(void *p) { free(p); }
static void vStreamBufferDeleteWithCaps(void *p) { free(p); }
typedef struct { int unused; } data_event_t;
static int xQueueReceive(void *q,void *p,int ticks) { (void)q;(void)p;(void)ticks;return 0; }
static void esp_opus_enc_close(void *p) { free(p); }
static void opus_decoder_destroy(void *p) { free(p); }
static void aec_destroy(void *p) { free(p); }
static void command_log_stats(const char *s) { (void)s; }
static void webrtc_peer_command_cancel(void) { }
static void webrtc_peer_group_set_sending(bool enabled,int deadline) { assert(!enabled&&!deadline); }
static void group_mark_unavailable(void) { }
static void log_heap_state(const char *phase,uint32_t cycle) { (void)phase;(void)cycle; }
static void rtc_alloc_diag_end(void) { }
static void drain_alloc_failures(const char *phase) { (void)phase; }
static bool command_active(void) { return fixture_command_active; }
static void webrtc_peer_stop(void);
static bool webrtc_peer_stopped(void);
static void peer_loop_task(void *);
static void data_event_task(void *);
static void *run_task(void *argument) { self=argument;self->fn(self->argument);assert(!"task returned");return NULL; }
static void vTaskDeleteWithCaps(void *handle) { assert(!handle&&self);atomic_store(&self->retired,true);pthread_exit(NULL); }
static int xTaskCreateWithCaps(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned priority,TaskHandle_t *out,unsigned caps) {
 if(!strcmp(name,"rti_peer")) {
  assert(fn==peer_loop_task&&stack==20*1024&&priority==RTC_PEER_TASK_PRIORITY);
  assert(caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
  if(fail_reserve||largest_internal<stack)return 0;
  reserved_stack=stack;largest_internal-=stack;
 } else {
  assert(!strcmp(name,"rti_events")&&fn==data_event_task&&stack==RTC_DATA_EVENT_STACK_SIZE);
  assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
  assert(!atomic_load(&loop_calls));
  if(fail_data)return 0;
 }
 TaskHandle_t t=calloc(1,sizeof(*t));assert(t&&task_count<4);t->fn=fn;t->argument=arg;tasks[task_count++]=t;*out=t;
 assert(!pthread_create(&t->thread,NULL,run_task,t));return pdPASS;
}
static void *esp_peer_get_default_impl(void) { return (void*)1; }
static int esp_peer_open(esp_peer_cfg_t *cfg,void *impl,void **peer) {
 assert(cfg&&impl==(void*)1);++open_calls;
 assert(reserved_stack==20*1024&&rtc.loop_task&&!atomic_load(&loop_calls));
 assert(!(rtc.events->bits&RTC_EVENT_LOOP_START)&&!rtc.running);
 if(fail_open)return -1;
 *peer=(void*)2;largest_internal=17408;return 0;
}
static int esp_peer_close(void *p) { assert(p==(void*)2);++close_calls;largest_internal=40960;return 0; }
static void esp_peer_main_loop(void *p) {
 assert(p==(void*)2);atomic_fetch_add(&loop_calls,1);
 while(atomic_load(&hold_loop))vTaskDelay(1);
}
'''

SUFFIX = r'''
static void data_event_task(void *arg) { (void)arg;while(rtc.running)vTaskDelay(1);finish_worker(RTC_EVENT_DATA_DONE); }
static void reset(bool group) {
 assert(!rtc.peer&&!rtc.events&&!rtc.lock);task_count=0;fail_reserve=fail_open=fail_data=false;
 atomic_store(&hold_loop,false);atomic_store(&hold_retirement,false);atomic_store(&loop_calls,0);atomic_store(&open_calls,0);atomic_store(&close_calls,0);
 reserved_stack=0;largest_internal=33792;
 rtc.group_mode=group;rtc.command_mode=!group;rtc.lock=malloc(1);rtc.events=calloc(1,sizeof(*rtc.events));assert(rtc.lock&&rtc.events);
 pthread_mutex_init(&rtc.events->mutex,NULL);pthread_cond_init(&rtc.events->changed,NULL);
 rtc.last_peer_state=-1;fixture_command_active=true;last_wait[0]=last_cleanup[0]=last_peer_data[0]=0;
}
static void join_retired(void) {
 for(unsigned i=0;i<task_count;++i){assert(!pthread_join(tasks[i]->thread,NULL));assert(atomic_load(&tasks[i]->retired));free(tasks[i]);}
 assert(!rtc.peer&&!rtc.events&&!rtc.lock&&!rtc.loop_task&&!rtc.data_task);
}
static void wait_for_loop(void) {
 TickType_t until=xTaskGetTickCount()+1000;while(!atomic_load(&loop_calls)&&xTaskGetTickCount()<until)vTaskDelay(1);assert(atomic_load(&loop_calls));
}
int main(void) {
 esp_peer_cfg_t config={0};
 // Exercise the real wait classifier; explicit failure is never a timeout,
 // even if READY is returned with FAILED. Cancellation is not a fabricated bit.
 reset(true);peer_cycle=41;rtc.last_peer_state=17;
 xEventGroupSetBits(rtc.events,RTC_EVENT_CONNECTED);
 assert(wait_peer_ready("channel_wait",RTC_EVENT_CONNECTED,10)==ESP_OK);
 assert(strstr(last_wait,"cycle=41 mode=group stage=channel_wait expected=0x080 returned=0x080 failed_bit=0 result=ready elapsed_ms="));
 assert(strstr(last_wait,"last_state=17"));
 xEventGroupSetBits(rtc.events,RTC_EVENT_FAILED);
 assert(wait_peer_ready("channel_wait",RTC_EVENT_CONNECTED,10)==ESP_FAIL);
 assert(strstr(last_wait,"returned=0x088 failed_bit=1 result=failed"));
 rtc.events->bits=RTC_EVENT_FAILED;
 assert(wait_peer_ready("local_sdp_wait",RTC_EVENT_LOCAL_SDP,10)==ESP_FAIL);
 assert(strstr(last_wait,"expected=0x001 returned=0x008 failed_bit=1 result=failed"));
 rtc.events->bits=0;
 assert(wait_peer_ready("local_sdp_wait",RTC_EVENT_LOCAL_SDP,10)==ESP_ERR_TIMEOUT);
 assert(strstr(last_wait,"returned=0x000 failed_bit=0 result=timeout"));
 rtc.command_mode=true;fixture_command_active=false;
 assert(wait_peer_ready("session_updated_wait",RTC_EVENT_COMMAND_READY,10)==ESP_ERR_INVALID_STATE);
 assert(strstr(last_wait,"mode=command stage=session_updated_wait expected=0x100 returned=0x000 failed_bit=0 result=cancelled"));
 webrtc_peer_stop();join_retired();
 reset(true);fail_reserve=true;
 assert(open_peer_transport(&config,1)==ESP_ERR_NO_MEM);assert(!atomic_load(&open_calls)&&!task_count);join_retired();
 reset(true);fail_open=true;
 assert(open_peer_transport(&config,2)==ESP_FAIL);assert(atomic_load(&open_calls)==1&&!atomic_load(&loop_calls));join_retired();
 reset(false);fail_data=true;
 assert(open_peer_transport(&config,3)==ESP_ERR_NO_MEM);assert(!atomic_load(&loop_calls)&&atomic_load(&close_calls)==1);join_retired();
 for(unsigned i=0;i<8;++i) {
  reset(i%2==0);assert(open_peer_transport(&config,4+i)==ESP_OK);wait_for_loop();
  assert(task_count==(rtc.group_mode?1:2));assert(!webrtc_peer_stopped());
  webrtc_peer_stop();assert(atomic_load(&close_calls)==1);assert(webrtc_peer_stopped());join_retired();
 }
 // A blocked owner retains its context; a later stop may safely release the
 // startup EventGroup again without notifying an already retired task handle.
 reset(true);atomic_store(&hold_loop,true);assert(open_peer_transport(&config,12)==ESP_OK);wait_for_loop();
 webrtc_peer_stop();assert(rtc.peer&&rtc.events&&rtc.lock&&!atomic_load(&close_calls));assert(!webrtc_peer_stopped());
 assert(strstr(last_peer_data,"stage=peer_data expected=0x004 observed=0x200 retired=0x000 result=timeout elapsed_ms="));
 assert(strstr(last_cleanup,"stage=cleanup expected=0x004 observed=0x200 retired=0x000 result=retained elapsed_ms="));
 atomic_store(&hold_loop,false);
 TickType_t until=xTaskGetTickCount()+1000;
 while(!(rtc.retired_workers&RTC_EVENT_LOOP_DONE)&&xTaskGetTickCount()<until)vTaskDelay(1);
 assert(rtc.retired_workers&RTC_EVENT_LOOP_DONE);
 webrtc_peer_stop();assert(atomic_load(&close_calls)==1);assert(webrtc_peer_stopped());join_retired();
 // DONE may wake stop while a worker is still inside EventGroupSetBits. It
 // cannot release owners until retirement is separately published.
 reset(true);atomic_store(&hold_retirement,true);assert(open_peer_transport(&config,13)==ESP_OK);wait_for_loop();
 webrtc_peer_stop();assert(rtc.peer&&rtc.events&&rtc.lock&&!atomic_load(&close_calls));assert(!webrtc_peer_stopped());
 assert(strstr(last_cleanup,"stage=cleanup expected=0x004 observed=0x204 retired=0x000 result=retained elapsed_ms="));
 atomic_store(&hold_retirement,false);
 until=xTaskGetTickCount()+1000;
 while(!(rtc.retired_workers&RTC_EVENT_LOOP_DONE)&&xTaskGetTickCount()<until)vTaskDelay(1);
 webrtc_peer_stop();assert(atomic_load(&close_calls)==1);
 assert(strstr(last_cleanup,"stage=cleanup expected=0x004 observed=0x204 retired=0x004 result=safe_to_release elapsed_ms="));
 join_retired();
 // A partial context with only a codec still needs real cleanup. An empty
 // peer/events/lock triple must not certify it as released.
 memset(&rtc,0,sizeof(rtc));rtc.opus_encoder=malloc(1);assert(rtc.opus_encoder);
 assert(!webrtc_peer_stopped());webrtc_peer_stop();assert(webrtc_peer_stopped());
 puts("Peer startup: 20KiB internal reservation/gate preserved; ready/failed/timeout/cancel diagnostics and observed-before-retired owner retention passed");
 return 0;
}
'''


def main():
    project = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    cc = shutil.which('cc')
    if not cc:
        parser.error('system cc required')
    source = (project / 'main/webrtc_peer.c').read_text()
    defines = '\n'.join(re.findall(r'^#define RTC_(?:EVENT_\w+|PEER_STACK_SIZE|PEER_TASK_PRIORITY|DATA_EVENT_STACK_SIZE|DATA_EVENT_TASK_PRIORITY) .+$', source, re.M))
    defines = re.sub(r'\bBIT(\d+)\b', lambda m: '(1U << ' + m[1] + ')', defines)
    # These are the production allocation, startup gate, retirement and cleanup
    # functions, not a second implementation of their lifecycle.
    functions = '\n\n'.join(extract_function(source, name) for name in (
        'peer_mode', 'peer_last_state', 'wait_peer_bits', 'wait_peer_ready',
        'finish_worker', 'fail_peer_start', 'peer_loop_task', 'log_stop_wait',
        'webrtc_peer_stopped', 'webrtc_peer_stop', 'open_peer_transport'))
    code = PREFIX.replace('/* @production-defines */', defines) + '\n' + functions + '\n' + SUFFIX
    flags = ['-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra', '-Werror', '-pthread']
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    with tempfile.TemporaryDirectory(prefix='intercom-peer-startup-') as directory:
        temp = Path(directory)
        (temp / 'fixture.c').write_text(code)
        subprocess.run([cc, *flags, str(temp / 'fixture.c'), '-o', str(temp / 'fixture')], check=True)
        subprocess.run([str(temp / 'fixture')], check=True, timeout=15)


if __name__ == '__main__':
    main()

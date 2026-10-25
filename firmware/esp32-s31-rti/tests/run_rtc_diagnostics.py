#!/usr/bin/env python3
"""Exercise the real bounded heap-failure hook and lifecycle log drain on host.

Injects capacity/lock contention, process-wide active-window scope, and concurrent
callbacks. This is an atomic recorder test, not an ESP heap or RTC reproduction.
"""
from pathlib import Path
import argparse
import shutil
import subprocess
import tempfile

from run_rta_command import extract_function


PREFIX = r'''
#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rtc_alloc_diagnostics.h"
static _Thread_local bool in_callback;
static void vTaskDelay(unsigned ticks) { assert(!in_callback&&ticks==1);sched_yield(); }
/* @production-recorder */
typedef int esp_err_t;
#define ESP_OK 0
#define MALLOC_CAP_INTERNAL 1U
#define MALLOC_CAP_8BIT 2U
#define MALLOC_CAP_DMA 4U
#define MALLOC_CAP_SPIRAM 8U
static const char *TAG = "test";
static unsigned registration_calls, log_count, heap_queries;
static char logs[32][512];
static void (*registered_callback)(size_t,uint32_t,const char *);
static esp_err_t heap_caps_register_failed_alloc_callback(void (*callback)(size_t,uint32_t,const char *)) {
 assert(!in_callback);++registration_calls;registered_callback=callback;return ESP_OK;
}
static const char *esp_err_to_name(esp_err_t error) { assert(!in_callback&&!error);return "ESP_OK"; }
static size_t heap_caps_get_free_size(uint32_t caps) { assert(!in_callback);++heap_queries;return 1000+caps; }
static size_t heap_caps_get_largest_free_block(uint32_t caps) { assert(!in_callback);++heap_queries;return 500+caps; }
static void quiet_log(const char *tag,const char *format,...) {
 assert(!in_callback&&tag&&log_count<32);va_list args;va_start(args,format);
 vsnprintf(logs[log_count++],sizeof(logs[0]),format,args);va_end(args);
}
#define ESP_LOGI(...) quiet_log(__VA_ARGS__)
#define ESP_LOGW(...) quiet_log(__VA_ARGS__)
'''


SUFFIX = r'''
static void fail_allocation(size_t size,uint32_t caps) {
 assert(registered_callback);in_callback=true;
 registered_callback(size,caps,"unretained_sdk_function");in_callback=false;
}
enum { PRODUCERS=4, CALLS=20000 };
static atomic_uint producers_done;
static void *producer(void *argument) {
 const unsigned id=(unsigned)(uintptr_t)argument;
 for(unsigned i=0;i<CALLS;++i) {
  const uint32_t request=(id<<20)|i;
  fail_allocation(request,request^0x7a5U);
 }
 atomic_fetch_add(&producers_done,1);return NULL;
}
static unsigned check_snapshot(const rtc_alloc_diag_snapshot_t *snapshot) {
 assert(snapshot->count<=RTC_ALLOC_DIAG_PENDING_LIMIT);
 for(unsigned i=0;i<snapshot->count;++i) {
  const rtc_alloc_diag_sample_t *sample=&snapshot->samples[i];
  assert(sample->cycle==77&&sample->mode==RTC_ALLOC_DIAG_GROUP);
  assert(sample->caps==((uint32_t)sample->size^0x7a5U));
  assert((sample->size>>20)<PRODUCERS&&(sample->size&0xfffffU)<CALLS);
 }
 return snapshot->count;
}
int main(void) {
 rtc_alloc_diag_snapshot_t snapshot;
 prepare_alloc_diagnostics();prepare_alloc_diagnostics();
 assert(registration_calls==1&&registered_callback==rtc_alloc_diag_failed);
 assert(strstr(logs[0],"RTC alloc_hook status=registered err=ESP_OK pending_limit=8"));
 fail_allocation(99,1);rtc_alloc_diag_take(&snapshot);assert(!snapshot.count&&!snapshot.dropped_total);
 rtc_alloc_diag_begin(12,RTC_ALLOC_DIAG_CONVERSATION);
 for(unsigned i=0;i<10;++i)fail_allocation(1024+i,3);
 assert(!heap_queries&&log_count==1);
 drain_alloc_failures("failure_injected");
 assert(log_count==10&&heap_queries==8*8);
 assert(strstr(logs[1],"RTC alloc_sampling phase=failure_injected scope=process_active_rtc policy=first_pending pending_limit=8 sampled=8 dropped_total=2 drop_scope=boot_active_windows"));
 assert(strstr(logs[2],"RTC alloc_failure cycle=12 mode=conversation phase=failure_injected size_bytes=1024 caps=0x00000003 memory_time=drain free_caps=1003 largest_caps=503"));
 assert(strstr(logs[9],"size_bytes=1031"));
 rtc_alloc_diag_take(&snapshot);assert(!snapshot.count&&snapshot.dropped_total==2);
 // A callback interrupting a lifecycle drain must immediately drop, not wait.
 assert(!atomic_flag_test_and_set(&sample_lock));fail_allocation(2000,7);atomic_flag_clear(&sample_lock);
 rtc_alloc_diag_take(&snapshot);assert(!snapshot.count&&snapshot.dropped_total==3);
 // Each stored sample owns its sampling-window metadata across later starts.
 fail_allocation(2001,7);rtc_alloc_diag_end();
 for(unsigned i=0;i<100;++i)fail_allocation(i,1);
 rtc_alloc_diag_begin(13,RTC_ALLOC_DIAG_COMMAND);fail_allocation(2002,5);
 rtc_alloc_diag_end();rtc_alloc_diag_take(&snapshot);
 assert(snapshot.count==2&&snapshot.dropped_total==3);
 assert(snapshot.samples[0].cycle==12&&snapshot.samples[0].mode==RTC_ALLOC_DIAG_CONVERSATION);
 assert(snapshot.samples[1].cycle==13&&snapshot.samples[1].mode==RTC_ALLOC_DIAG_COMMAND);
 // Several allocating tasks race each other and the drain; every active
 // callback must become either one intact sample or one counted drop.
 rtc_alloc_diag_begin(77,RTC_ALLOC_DIAG_GROUP);pthread_t threads[PRODUCERS];
 for(unsigned i=0;i<PRODUCERS;++i)assert(!pthread_create(&threads[i],NULL,producer,(void *)(uintptr_t)i));
 unsigned sampled=0;
 while(atomic_load(&producers_done)!=PRODUCERS) {
  rtc_alloc_diag_take(&snapshot);sampled+=check_snapshot(&snapshot);sched_yield();
 }
 for(unsigned i=0;i<PRODUCERS;++i)assert(!pthread_join(threads[i],NULL));
 rtc_alloc_diag_end();rtc_alloc_diag_take(&snapshot);sampled+=check_snapshot(&snapshot);
 assert(sampled+snapshot.dropped_total-3==PRODUCERS*CALLS);
 assert(heap_queries==8*8&&log_count==10);
 printf("RTC allocation diagnostics: bounded samples, contention drops, inactive windows, log-time heaps and %u concurrent callbacks accounted for\n",PRODUCERS*CALLS);
 return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    cc = shutil.which('cc')
    if not cc:
        parser.error('system cc required')
    project = Path(__file__).resolve().parents[1]
    source = (project / 'main/webrtc_peer.c').read_text()
    functions = '\n\n'.join(extract_function(source, name) for name in (
        'alloc_diag_mode', 'drain_alloc_failures', 'prepare_alloc_diagnostics'))
    code = PREFIX.replace('/* @production-recorder */', '#include "rtc_alloc_diagnostics.c"')
    code += '\n' + functions + '\n' + SUFFIX
    flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread']
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    with tempfile.TemporaryDirectory(prefix='intercom-rtc-diagnostics-') as directory:
        temp = Path(directory)
        (temp / 'freertos').mkdir()
        (temp / 'esp_attr.h').write_text('#define IRAM_ATTR\n')
        (temp / 'freertos/FreeRTOS.h').write_text('')
        (temp / 'freertos/task.h').write_text('')
        (temp / 'fixture.c').write_text(code)
        subprocess.run([cc, *flags, '-I', str(temp), '-I', str(project / 'main'),
                        str(temp / 'fixture.c'), '-o', str(temp / 'fixture')], check=True)
        subprocess.run([str(temp / 'fixture')], check=True, timeout=15)


if __name__ == '__main__':
    main()

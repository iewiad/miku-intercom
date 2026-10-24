#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RTC_ALLOC_DIAG_PENDING_LIMIT 8

typedef enum {
    RTC_ALLOC_DIAG_CONVERSATION,
    RTC_ALLOC_DIAG_GROUP,
    RTC_ALLOC_DIAG_COMMAND,
} rtc_alloc_diag_mode_t;

typedef struct {
    size_t size;
    uint32_t caps;
    uint32_t cycle;
    rtc_alloc_diag_mode_t mode;
} rtc_alloc_diag_sample_t;

typedef struct {
    rtc_alloc_diag_sample_t samples[RTC_ALLOC_DIAG_PENDING_LIMIT];
    unsigned count;
    unsigned dropped_total;
} rtc_alloc_diag_snapshot_t;

/* Serialized lifecycle-task calls only. Sampling covers process-wide allocation
 * failures while a Peer context is active, including retained teardown. A cycle
 * labels the sampling window, not the allocating task or the cause of a fault.
 * Pending samples survive scope changes; first eight pending failures win.
 * dropped_total is cumulative across active windows since boot, not per cycle.
 */
void rtc_alloc_diag_begin(uint32_t cycle, rtc_alloc_diag_mode_t mode);
void rtc_alloc_diag_end(void);
void rtc_alloc_diag_take(rtc_alloc_diag_snapshot_t *snapshot);

/* IDF esp_alloc_failed_hook_t-compatible callback: bounded, no waiting,
 * allocation, logging, clock reads, heap queries, or task/heap locks. The SDK
 * function name is intentionally not retained. Contention/full samples drop.
 */
void rtc_alloc_diag_failed(size_t size, uint32_t caps, const char *function_name);

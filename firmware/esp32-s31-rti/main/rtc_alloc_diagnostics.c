#include "rtc_alloc_diagnostics.h"

#include <stdatomic.h>
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Fail the build rather than introduce a hidden libatomic lock in the hook.
 * atomic_flag itself is required to be lock-free by C11.
 */
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "allocation diagnostics require lock-free unsigned atomics");
_Static_assert(ATOMIC_BOOL_LOCK_FREE == 2, "allocation diagnostics require lock-free boolean atomics");

static atomic_flag sample_lock = ATOMIC_FLAG_INIT;
static atomic_bool active;
static atomic_uint dropped_total;
static uint32_t active_cycle;
static rtc_alloc_diag_mode_t active_mode;
static unsigned pending_count;
static rtc_alloc_diag_sample_t pending[RTC_ALLOC_DIAG_PENDING_LIMIT];

/* Only task lifecycle callers wait for this tiny record-copy critical section.
 * The callback always tries once and returns on contention, including when it
 * interrupts a task holding sample_lock. Never call lifecycle APIs in the hook.
 */
static void lifecycle_lock(void)
{
    while (atomic_flag_test_and_set_explicit(&sample_lock, memory_order_acquire)) vTaskDelay(1);
}

void rtc_alloc_diag_begin(uint32_t cycle, rtc_alloc_diag_mode_t mode)
{
    lifecycle_lock();
    active_cycle = cycle;
    active_mode = mode;
    atomic_store_explicit(&active, true, memory_order_relaxed);
    atomic_flag_clear_explicit(&sample_lock, memory_order_release);
}

void rtc_alloc_diag_end(void)
{
    lifecycle_lock();
    atomic_store_explicit(&active, false, memory_order_relaxed);
    atomic_flag_clear_explicit(&sample_lock, memory_order_release);
}

void rtc_alloc_diag_take(rtc_alloc_diag_snapshot_t *snapshot)
{
    lifecycle_lock();
    snapshot->count = pending_count;
    for (unsigned i = 0; i < pending_count; ++i) snapshot->samples[i] = pending[i];
    pending_count = 0;
    snapshot->dropped_total = atomic_load_explicit(&dropped_total, memory_order_relaxed);
    atomic_flag_clear_explicit(&sample_lock, memory_order_release);
}

void IRAM_ATTR rtc_alloc_diag_failed(size_t size, uint32_t caps, const char *function_name)
{
    (void)function_name;
    if (!atomic_load_explicit(&active, memory_order_relaxed)) return;
    if (atomic_flag_test_and_set_explicit(&sample_lock, memory_order_acquire)) {
        atomic_fetch_add_explicit(&dropped_total, 1, memory_order_relaxed);
        return;
    }
    if (atomic_load_explicit(&active, memory_order_relaxed)) {
        if (pending_count < RTC_ALLOC_DIAG_PENDING_LIMIT) {
            pending[pending_count++] = (rtc_alloc_diag_sample_t) {
                .size = size, .caps = caps, .cycle = active_cycle, .mode = active_mode,
            };
        } else {
            atomic_fetch_add_explicit(&dropped_total, 1, memory_order_relaxed);
        }
    }
    atomic_flag_clear_explicit(&sample_lock, memory_order_release);
}

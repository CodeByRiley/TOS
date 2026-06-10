/* userspace/lib/time_stub.c — coarse `time()` from the kernel tick counter.
 *
 * No RTC syscall yet, so wall-clock time is fake: it just exposes uptime
 * in seconds. Apps that need monotonic timing get something usable; apps
 * that expect a real Unix epoch will misbehave.
 */
#include "../include/time.h"
#include "syscall.h"

/* Returns uptime in seconds. If `t` is non-NULL, also stores the value. */
time_t time(time_t *t) {
    time_t v = get_ticks() / 1000;
    if (t) *t = v;
    return v;
}

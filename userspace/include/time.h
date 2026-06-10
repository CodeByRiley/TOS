/* userspace/include/time.h — minimal <time.h> surface.
 *
 * `time()` returns seconds of uptime (not real wall-clock — there's no
 * RTC syscall yet). See lib/time_stub.c. Grow this header when an app
 * needs more than time_t.
 */
#ifndef TIME_H
#define TIME_H

typedef long time_t;

time_t time(time_t *t);

#endif

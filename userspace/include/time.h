/* userspace/include/time.h — minimal <time.h> surface.
 *
 * `time()` is real wall-clock now: the kernel reads the CMOS RTC and
 * CLOCK_REALTIME reports seconds since the Unix epoch. It used to return
 * uptime because there was no RTC at all.
 *
 * Whether that clock is UTC or local time is a property of the machine's
 * CMOS, not something the kernel can know. QEMU defaults to UTC; its
 * `-rtc base=localtime` switches it.
 *
 * Implementations: lib/time_stub.c (time, clock_gettime, civil conversion).
 *
 * A musl-linked binary gets time_t, struct timespec, time() and
 * clock_gettime() from the real <time.h>; redeclaring them here would be a
 * struct redefinition, so only the TOS-only calendar helper below survives.
 * Such a binary still has to reach this header by its full path
 * (<include/time.h>) because a plain <time.h> resolves to musl's.
 */
#ifndef TIME_H
#define TIME_H

#ifdef TOS_USE_MUSL
#include <time.h>
#else

#include <stddef.h>

typedef long time_t;

/* Matches the kernel's struct linux_timespec byte for byte. */
struct timespec {
    long tv_sec;
    long tv_nsec;
};

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

/* Seconds since the epoch, and 0 if the clock is unreadable. Also stored
 * through `t` when non-NULL. */
time_t time(time_t *t);

/* 0 on success, -1 on a bad clock id or pointer. CLOCK_MONOTONIC is uptime;
 * CLOCK_REALTIME is wall clock. */
int clock_gettime(int clock_id, struct timespec *ts);

#endif /* TOS_USE_MUSL */

/* Broken-down calendar time, in whatever zone the RTC is keeping. Fields
 * follow the obvious ranges rather than struct tm's offsets — year is
 * absolute, month is 1-12 — because every caller here formats them directly
 * and tm_year + 1900 is a reliable source of off-by-1900 bugs. */
struct calendar_time {
    int year;    /* e.g. 2026 */
    int month;   /* 1-12     */
    int day;     /* 1-31     */
    int hour;    /* 0-23     */
    int minute;  /* 0-59     */
    int second;  /* 0-59     */
    int weekday; /* 0 = Sunday */
};

/* Split an epoch timestamp into calendar fields. No timezone handling: the
 * value is interpreted as-is. */
void time_to_calendar(time_t epoch, struct calendar_time *out);

#endif

/* userspace/lib/time_stub.c — wall-clock time from the kernel's RTC.
 *
 * `time()` used to return uptime because there was no RTC at all. The kernel
 * reads the CMOS now, so CLOCK_REALTIME is a real Unix timestamp and this is
 * a thin wrapper over it rather than a stand-in.
 */
#include <include/time.h>
#include <lib/syscall.h>

int clock_gettime(int clock_id, struct timespec *ts) {
    if (!ts)
        return -1;
    return (int)sys_clock_gettime(clock_id, ts);
}

time_t time(time_t *t) {
    struct timespec ts = {0, 0};
    time_t v = 0;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0)
        v = ts.tv_sec;
    if (t)
        *t = v;
    return v;
}

/* Inverse of the kernel's days_from_civil. Shifts the year to start in March
 * so February's variable length falls at the end of the 400-year cycle and no
 * month-length table is needed. */
static void civil_from_days(long days, int *year, int *month, int *day) {
    days += 719468;
    long era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned long doe = (unsigned long)(days - era * 146097);      /* 0..146096 */
    unsigned long yoe =
        (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;     /* 0..399    */
    long y = (long)yoe + era * 400;
    unsigned long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);   /* 0..365    */
    unsigned long mp = (5 * doy + 2) / 153;                        /* 0..11     */
    unsigned long d = doy - (153 * mp + 2) / 5 + 1;                /* 1..31     */
    unsigned long m = mp + (mp < 10 ? 3 : -9);                     /* 1..12     */

    *year = (int)(y + (m <= 2));
    *month = (int)m;
    *day = (int)d;
}

void time_to_calendar(time_t epoch, struct calendar_time *out) {
    if (!out)
        return;

    long days = (long)(epoch / 86400);
    long rem = (long)(epoch % 86400);
    if (rem < 0) { /* keep the time-of-day positive for pre-epoch values */
        rem += 86400;
        days -= 1;
    }

    civil_from_days(days, &out->year, &out->month, &out->day);
    out->hour = (int)(rem / 3600);
    out->minute = (int)((rem % 3600) / 60);
    out->second = (int)(rem % 60);
    /* 1970-01-01 was a Thursday, hence the +4 before reducing mod 7. */
    out->weekday = (int)(((days % 7) + 11) % 7);
}

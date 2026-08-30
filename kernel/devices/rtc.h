/* kernel/devices/rtc.h , CMOS real-time clock.
 *
 * The only wall-clock source in the system. Everything else that looks like
 * a clock is uptime: pit_ticks() counts from boot, so current_timespec() and
 * therefore gettimeofday() report seconds since power-on rather than since
 * the epoch. Filesystem timestamps need a real date, which means the CMOS.
 *
 * Implementation: kernel/devices/rtc.c.
 */
#ifndef RTC_H
#define RTC_H

#include <utilities/types.h>
#include <stdint.h>

/* Broken-down local time as the CMOS reports it. `year` is absolute
 * (2026, not 26). All fields are zero if the clock could not be read. */
struct rtc_time {
    u16 year;
    u8 month;   /* 1-12 */
    u8 day;     /* 1-31 */
    u8 hour;    /* 0-23 */
    u8 minute;  /* 0-59 */
    u8 second;  /* 0-59 */
    u8 valid;   /* 0 when the read failed or the clock is implausible */
};

/* Read the current time. Handles the update-in-progress flag, BCD encoding,
 * and 12-hour mode. Cheap enough to call per filesystem operation: a few
 * port reads, no allocation, no locks. */
void rtc_read(struct rtc_time *out);

/* Seconds since 1970-01-01 00:00:00 as the CMOS reports it, or 0 when the
 * clock is unreadable. Whether that is UTC or local time is a property of
 * the machine's CMOS, not something this kernel can know , QEMU defaults to
 * UTC and `-rtc base=localtime` switches it. */
u64 rtc_unix_epoch(void);

#endif

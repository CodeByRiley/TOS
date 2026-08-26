/* kernel/devices/rtc.c , CMOS real-time clock.
 *
 * Reads the MC146818-compatible RTC behind ports 0x70 (register select) and
 * 0x71 (data). QEMU presents the host clock here, so this is the only place
 * the kernel learns the actual date.
 */
#include <devices/io.h>
#include <devices/rtc.h>

#define CMOS_SELECT 0x70
#define CMOS_DATA   0x71

#define CMOS_SECOND  0x00
#define CMOS_MINUTE  0x02
#define CMOS_HOUR    0x04
#define CMOS_DAY     0x07
#define CMOS_MONTH   0x08
#define CMOS_YEAR    0x09
#define CMOS_STATUS_A 0x0A
#define CMOS_STATUS_B 0x0B

#define STATUS_A_UPDATING 0x80 /* set while the clock is mid-update       */
#define STATUS_B_BINARY   0x04 /* set: values are binary, clear: BCD      */
#define STATUS_B_24HOUR   0x02 /* set: 24-hour, clear: 12-hour with PM bit */

#define HOUR_PM_FLAG 0x80

/* Bit 7 of the select port is the NMI-disable line on most chipsets. Leave
 * it clear so reading the clock does not mask NMIs , the panic path relies on
 * those being delivered. */
static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_SELECT, reg & 0x7F);
    return inb(CMOS_DATA);
}

static int rtc_updating(void) {
    return (cmos_read(CMOS_STATUS_A) & STATUS_A_UPDATING) != 0;
}

static uint8_t from_bcd(uint8_t v) {
    return (uint8_t)((v & 0x0F) + ((v >> 4) * 10));
}

/* One raw sample of the six time registers. */
static void sample(struct rtc_time *t, uint8_t *hour_raw) {
    t->second = cmos_read(CMOS_SECOND);
    t->minute = cmos_read(CMOS_MINUTE);
    *hour_raw = cmos_read(CMOS_HOUR);
    t->day = cmos_read(CMOS_DAY);
    t->month = cmos_read(CMOS_MONTH);
    t->year = cmos_read(CMOS_YEAR);
    t->hour = *hour_raw;
}

void rtc_read(struct rtc_time *out) {
    if (!out)
        return;

    struct rtc_time a, b;
    uint8_t hour_raw = 0, hour_raw_b = 0;

    /* Wait out any update in progress, then sample twice and require the two
     * to agree. A single read can straddle the chip's own carry , 01:59:59
     * becoming 01:00:00 rather than 02:00:00 , and the second sample is what
     * rules that out. The bound keeps a dead or emulated-away clock from
     * hanging the caller. */
    int spins = 0;
    while (rtc_updating() && ++spins < 1000000)
        ;

    for (int attempt = 0; attempt < 8; attempt++) {
        sample(&a, &hour_raw);
        sample(&b, &hour_raw_b);
        if (a.second == b.second && a.minute == b.minute &&
            hour_raw == hour_raw_b && a.day == b.day && a.month == b.month &&
            a.year == b.year)
            break;
    }

    uint8_t status_b = cmos_read(CMOS_STATUS_B);

    if (!(status_b & STATUS_B_BINARY)) {
        a.second = from_bcd(a.second);
        a.minute = from_bcd(a.minute);
        a.day = from_bcd(a.day);
        a.month = from_bcd(a.month);
        a.year = from_bcd((uint8_t)a.year);
        /* The PM flag lives in the top bit and must survive the conversion,
         * so strip it first and put it back after. */
        a.hour = (uint8_t)(from_bcd((uint8_t)(hour_raw & 0x7F)) |
                           (hour_raw & HOUR_PM_FLAG));
    }

    if (!(status_b & STATUS_B_24HOUR) && (a.hour & HOUR_PM_FLAG)) {
        a.hour = (uint8_t)(((a.hour & 0x7F) % 12) + 12);
    } else {
        a.hour &= 0x7F;
    }

    /* The year register holds two digits and there is no reliable century
     * register across chipsets, so pivot: values below 80 are 21st century.
     * Same convention FAT itself uses by counting from 1980. */
    uint16_t year2 = (uint16_t)(a.year % 100);
    out->year = (uint16_t)(year2 < 80 ? 2000 + year2 : 1900 + year2);
    out->month = a.month;
    out->day = a.day;
    out->hour = a.hour;
    out->minute = a.minute;
    out->second = a.second;

    /* Sanity-check rather than trust it. A clock that reports month 0 or day
     * 0 would encode as a FAT date that tools reject outright, and a caller
     * checking `valid` can fall back to leaving the field alone. */
    out->valid = (out->month >= 1 && out->month <= 12 && out->day >= 1 &&
                  out->day <= 31 && out->hour < 24 && out->minute < 60 &&
                  out->second < 60 && out->year >= 1980 && out->year < 2200);
}

/* Days from 1970-01-01 to y-m-d, proleptic Gregorian, valid for any year the
 * RTC can report. Shifts the year to start in March so the leap day lands at
 * the end of the cycle and no month-length table is needed; 719468 is the
 * day offset between the era's zero and the Unix epoch. */
static int64_t days_from_civil(int32_t y, uint32_t m, uint32_t d) {
    y -= (m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);                  /* 0..399   */
    uint32_t doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;   /* 0..146096 */
    return era * 146097 + (int64_t)doe - 719468;
}

uint64_t rtc_unix_epoch(void) {
    struct rtc_time now;
    rtc_read(&now);
    if (!now.valid)
        return 0;

    int64_t days = days_from_civil(now.year, now.month, now.day);
    int64_t secs = days * 86400 + now.hour * 3600 + now.minute * 60 + now.second;
    return secs < 0 ? 0 : (uint64_t)secs;
}

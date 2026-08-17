/* tests/host_kernel_stubs.c — host-side stand-ins for kernel services.
 *
 * Kernel sources compiled into a host test still reference the allocator
 * and the log. Rather than have every test carry its own copy of the same
 * four one-liners, they link this.
 *
 * These are stubs, not fakes: the allocator is plain malloc/free and the
 * log goes nowhere. A test that wants to assert on either should define
 * its own and leave this file out of its link line.
 */
#include <devices/rtc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void *kmalloc(size_t n) { return malloc(n); }

void kfree(void *p) { free(p); }

void log_write(const char *message, uint8_t type, uint8_t level) {
    (void)message;
    (void)type;
    (void)level;
}

void log_write_hex(const char *message, uint64_t value,
                   uint8_t type, uint8_t level) {
    (void)message;
    (void)value;
    (void)type;
    (void)level;
}

/* No CMOS on the host, and a test that asserted on the wall clock would fail
 * once a day anyway. Reporting invalid makes fat_set_timestamp leave the date
 * fields alone, which is the behaviour the directory tests already expect. */
void rtc_read(struct rtc_time *out) {
    if (out)
        memset(out, 0, sizeof(*out));
}

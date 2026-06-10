/* userspace/lib/unistd_stub.c — minimal unistd surface.
 *
 * Most POSIX bits aren't wired up yet; this file gives callers something
 * that compiles + behaves harmlessly. sleep/usleep busy-wait against the
 * tick counter — fine for the current single-task workloads.
 */
#include "../include/unistd.h"
#include "syscall.h"

/* TTY detection unsupported — treat every fd as a pipe/file. */
int isatty(int fd)                       { (void)fd; return 0; }

/* No cwd concept yet — fail. */
int chdir(const char *p)                { (void)p; return -1; }

/* Returns an empty string; cwd is effectively unknown. */
char *getcwd(char *buf, size_t size)    { if (buf && size) buf[0] = 0; return buf; }

/* Busy-wait `s` seconds. Returns 0 (no signal handling). */
unsigned int sleep(unsigned int s) {
    long target = get_ticks() + (long)s * 1000;
    while (get_ticks() < target) { }
    return 0;
}

/* Busy-wait `us` microseconds (resolution is actually ~1 ms — tick rate). */
int usleep(unsigned int us) {
    long target = get_ticks() + (long)(us / 1000);
    while (get_ticks() < target) { }
    return 0;
}

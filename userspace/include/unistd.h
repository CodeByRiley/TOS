/* userspace/include/unistd.h — POSIX unistd surface.
 *
 * The big four (read/write/open/close/lseek/unlink) live in syscall.h so
 * the syscall wrappers and the public POSIX prototype stay in lockstep.
 * Only the helpers that don't have a syscall analogue are declared here.
 */
#ifndef UNISTD_H
#define UNISTD_H

#include <stddef.h>
#include "../lib/syscall.h"

/* TTY detection — currently always returns 0 (no isatty support). */
int    isatty(int fd);

/* CWD ops — no concept of cwd yet, both return failure / empty. */
int    chdir(const char *path);
char  *getcwd(char *buf, size_t size);

/* Busy-wait sleeps against the tick counter. */
unsigned int sleep(unsigned int seconds);
int    usleep(unsigned int us);

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#endif

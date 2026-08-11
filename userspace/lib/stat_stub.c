/* userspace/lib/stat_stub.c — stat/fstat stubs plus mkdir.
 *
 * Filesystem metadata isn't exposed through syscalls yet. Apps that ask
 * for it get -1 so they fall back to their "no info" path. Replace once
 * the kernel grows real stat support.
 */
#include "../include/sys/stat.h"
#include "syscall.h"

/* Always -1: stat info not supported. */
int stat(const char *path, struct stat *buf)  { (void)path; (void)buf; return -1; }

/* Always -1: fstat not supported. */
int fstat(int fd, struct stat *buf)            { (void)fd; (void)buf; return -1; }

int mkdir(const char *path, mode_t mode) {
    (void)mode;
    return (int)mkdir_path(path);
}

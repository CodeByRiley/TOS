#include "syscall.h"

long write(int fd, const void *buf, size_t n) {
    return syscall3(SYS_WRITE, fd, (long)buf, (long)n);
}

long read(int fd, void *buf, size_t n) {
    return syscall3(SYS_READ, fd, (long)buf, (long)n);
}

long open(const char *path, int flags) {
    return syscall2(SYS_OPEN, (long)path, flags);
}

long close(int fd) {
    return syscall1(SYS_CLOSE, fd);
}

long lseek(int fd, long off, int whence) {
    return syscall3(SYS_LSEEK, fd, off, whence);
}

long readdir(unsigned *index, char *buf, size_t n) {
    return syscall3(SYS_READDIR, (long)index, (long)buf, (long)n);
}

long unlink(const char *path) {
    return syscall1(SYS_UNLINK, (long)path);
}

void exit(int code) {
    syscall1(SYS_EXIT, code);
    for (;;);   /* unreachable but compiler doesn't know */
}

long fb_info(struct fb_info *out) {
    return syscall1(SYS_FB_INFO, (long)out);
}

void *fb_map(void) {
    return (void*)syscall0(SYS_FB_MAP);
}

long kbd_poll(int *pressed, uint16_t *key) {
    return syscall2(SYS_KBD_POLL, (long)pressed, (long)key);
}

long get_ticks(void) {
    return syscall0(SYS_GET_TICKS);
}

long exec(const char *path, char *const argv[]) {
    /* runs child synchronously; returns child's exit code (or -1 on fail) */
    return syscall2(SYS_EXEC, (long)path, (long)argv);
}

long sys_shutdown(int time, const char *reason) {
    return syscall2(SYS_SHUTDOWN, time, (long)reason);
}

long sys_reboot(int time) {
    return syscall1(SYS_REBOOT, time);
}

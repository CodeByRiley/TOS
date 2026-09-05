/* userspace/lib/syscall_posix.c , POSIX syscall wrappers.
 *
 * Split out of syscall.c because musl defines every one of these. A
 * binary linked against musl uses musl's versions and must not link this
 * file; libtos.a is built from the rest of lib/, which collides with
 * nothing in libc.a.
 *
 * The hand-rolled libc build links this exactly as before.
 */
#include <lib/syscall.h>

/* File / I/O */
long write(int fd, const void *buf, size_t n) {
    return syscall3(SYS_WRITE, fd, (sysarg_t)(uintptr_t)buf, (sysarg_t)(uintptr_t)n);
}
long read(int fd, void *buf, size_t n) {
    return syscall3(SYS_READ, fd, (sysarg_t)(uintptr_t)buf, (sysarg_t)(uintptr_t)n);
}
long open(const char *path, int flags) {
    return syscall2(SYS_OPEN, (sysarg_t)(uintptr_t)path, flags);
}
long close(int fd) {
    return syscall1(SYS_CLOSE, fd);
}
long lseek(int fd, long off, int whence) {
    return syscall3(SYS_LSEEK, fd, off, whence);
}
long chdir(const char *path) {
    return syscall1(SYS_CHDIR, (sysarg_t)(uintptr_t)path);
}
/* Mounting. musl declares these, so like the rest of this file they exist
 * only for the hand-rolled libc; a musl binary reaches the same syscalls
 * through musl's own wrappers. See lib/syscall.h for what TOS accepts. */
int mount(const char *source, const char *target, const char *filesystemtype,
          unsigned long mountflags, const void *data) {
    return (int)syscall5(SYS_MOUNT, (sysarg_t)(uintptr_t)source,
                         (sysarg_t)(uintptr_t)target,
                         (sysarg_t)(uintptr_t)filesystemtype,
                         (sysarg_t)mountflags, (sysarg_t)(uintptr_t)data);
}
int umount2(const char *target, int flags) {
    return (int)syscall2(SYS_UMOUNT, (sysarg_t)(uintptr_t)target,
                         (sysarg_t)flags);
}
int umount(const char *target) { return umount2(target, 0); }
char *getcwd(char *buf, size_t size) {
    long rc = syscall2(SYS_GETCWD, (sysarg_t)(uintptr_t)buf, (sysarg_t)size);
    return rc < 0 ? 0 : buf;
}
/* Memory
 *
 * MAP_FAILED rather than 0 on error: 0 is a legal-looking value for code
 * that only checks `!= NULL`, and a fixed mapping at a low address would
 * be indistinguishable from failure. */
void *mmap(void *addr, size_t len, int prot, int flags) {
    long rc = syscall4(SYS_MMAP, (sysarg_t)(uintptr_t)addr, (sysarg_t)len, prot, flags);
    return rc < 0 ? MAP_FAILED : (void *)(uintptr_t)rc;
}
int mprotect(void *addr, size_t len, int prot) {
    return (int)syscall3(SYS_MPROTECT, (sysarg_t)(uintptr_t)addr, (sysarg_t)len, prot);
}
int munmap(void *addr, size_t len) {
    return (int)syscall2(SYS_MUNMAP, (sysarg_t)(uintptr_t)addr, (sysarg_t)len);
}
long readdir(unsigned *index, char *buf, size_t n) {
    return syscall3(SYS_READDIR_INDEX, (sysarg_t)(uintptr_t)index,
                    (sysarg_t)(uintptr_t)buf, (sysarg_t)n);
}
long unlink(const char *path) {
    return syscall1(SYS_UNLINK, (sysarg_t)(uintptr_t)path);
}
/* Process control */
/* Never returns; the for-loop is dead code that quiets warnings. */
void exit(int code) {
    syscall1(SYS_EXIT, code);
    for (;;);
}
long kill(long pid, int signal) {
    return syscall2(SYS_KILL, pid, signal);
}

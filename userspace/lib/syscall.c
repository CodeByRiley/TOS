/* userspace/lib/syscall.c — typed C wrappers around syscallN().
 *
 * Each function loads the syscall number plus the right argument
 * combination, casts pointers through uintptr_t so calling conventions
 * stay clean, and returns the kernel's long result unchanged. Anything
 * that isn't a syscall (winman IPC helpers, etc.) lives elsewhere — see
 * lib/wm.c.
 */
#include "syscall.h"

/* --- File / I/O --------------------------------------------------------- */
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

char *getcwd(char *buf, size_t size) {
    long rc = syscall2(SYS_GETCWD, (sysarg_t)(uintptr_t)buf, (sysarg_t)size);
    return rc == 0 ? buf : 0;
}

long stat_raw(const char *path, struct stat_user *out) {
    return syscall2(SYS_STAT, (sysarg_t)(uintptr_t)path, (sysarg_t)(uintptr_t)out);
}

long fstat_raw(int fd, struct stat_user *out) {
    return syscall2(SYS_FSTAT, fd, (sysarg_t)(uintptr_t)out);
}

/* --- Memory -------------------------------------------------------------
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
    return syscall3(SYS_READDIR, (sysarg_t)(uintptr_t)index, (sysarg_t)(uintptr_t)buf, (sysarg_t)n);
}

long readdir_path(const char *path, unsigned *index, char *buf, size_t n) {
    return syscall4(SYS_READDIR_PATH, (sysarg_t)(uintptr_t)path,
                    (sysarg_t)(uintptr_t)index, (sysarg_t)(uintptr_t)buf, (sysarg_t)n);
}

long mkdir_path(const char *path) {
    return syscall1(SYS_MKDIR, (sysarg_t)(uintptr_t)path);
}

long unlink(const char *path) {
    return syscall1(SYS_UNLINK, (sysarg_t)(uintptr_t)path);
}

/* --- Process control ---------------------------------------------------- */
/* Never returns; the for-loop is dead code that quiets warnings. */
void exit(int code) {
    syscall1(SYS_EXIT, code);
    for (;;);
}

long yield(void) {
    return syscall0(SYS_YIELD);
}

/* --- Input event ring --------------------------------------------------- */
long msg_get(struct msg *out) {
    return syscall1(SYS_MSG_GET, (sysarg_t)(uintptr_t)out);
}

long msg_peek(struct msg *out) {
    return syscall1(SYS_MSG_PEEK, (sysarg_t)(uintptr_t)out);
}

long mouse_pos(int32_t *x, int32_t *y, uint8_t *buttons) {
    return syscall3(SYS_MOUSE_POS, (sysarg_t)(uintptr_t)x, (sysarg_t)(uintptr_t)y, (sysarg_t)(uintptr_t)buttons);
}

/* --- Framebuffer -------------------------------------------------------- */
long fb_info(struct fb_info *out) {
    return syscall1(SYS_FB_INFO, (sysarg_t)(uintptr_t)out);
}

void *fb_map(void) {
    return (void*)(uintptr_t)syscall0(SYS_FB_MAP);
}

long fb_damage(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    return syscall4(SYS_FB_DAMAGE, x, y, w, h);
}

long kbd_poll(int *pressed, uint16_t *key) {
    return syscall2(SYS_KBD_POLL, (sysarg_t)(uintptr_t)pressed, (sysarg_t)(uintptr_t)key);
}

long get_ticks(void) {
    return syscall0(SYS_GET_TICKS);
}

/* --- Child processes ---------------------------------------------------- */
/* Synchronous: blocks the caller until the child exits. Returns the
 * child's exit code (or -1 on failure to launch). */
long exec(const char *path, char *const argv[]) {
    return syscall2(SYS_EXEC, (sysarg_t)(uintptr_t)path, (sysarg_t)(uintptr_t)argv);
}

/* Fire-and-forget: returns the child's pid (or -1) without waiting.
 * Used by the shell for windowed apps so the prompt stays interactive. */
long spawn(const char *path, char *const argv[]) {
    return syscall2(SYS_SPAWN, (sysarg_t)(uintptr_t)path, (sysarg_t)(uintptr_t)argv);
}

long kill(long pid, int signal) {
    return syscall2(SYS_KILL, pid, signal);
}

long sys_shutdown(int time, const char *reason) {
    return syscall2(SYS_SHUTDOWN, time, (sysarg_t)(uintptr_t)reason);
}

long sys_reboot(int time) {
    return syscall1(SYS_REBOOT, time);
}

/* --- Console (TTY) ------------------------------------------------------ */
long con_write(const char *buf, size_t n) {
    return syscall2(SYS_CON_WRITE, (sysarg_t)(uintptr_t)buf, (sysarg_t)(uintptr_t)n);
}

long con_clear(void) {
    return syscall0(SYS_CON_CLEAR);
}

long con_push(void) {
    return syscall0(SYS_CON_PUSH);
}

long con_zoom(long delta) {
    return syscall1(SYS_CON_ZOOM, delta);
}

long con_pop(void) {
    return syscall0(SYS_CON_POP);
}

long sleep_ticks(unsigned long n) {
    return syscall1(SYS_SLEEP_TICKS, (sysarg_t)n);
}

long get_pid(void) {
    return syscall0(SYS_GET_PID);
}

/* --- IPC and shared memory --------------------------------------------- */
long ipc_send(int target_pid, const struct ipc_msg *m) {
    return syscall2(SYS_IPC_SEND, target_pid, (sysarg_t)(uintptr_t)m);
}

long ipc_recv(struct ipc_msg *out) {
    return syscall1(SYS_IPC_RECV, (sysarg_t)(uintptr_t)out);
}

/* Map a contiguous range of pages from `my_va` into the target's address
 * space. Kernel chooses the target va and writes it back to out_target_va. */
long shmem_share(int target_pid, uint64_t my_va, long npages,
                 uint64_t *out_target_va) {
    return syscall4(SYS_SHMEM_SHARE, target_pid, (sysarg_t)my_va, npages,
                    (sysarg_t)(uintptr_t)out_target_va);
}

long shmem_unshare(int target_pid, uint64_t my_va, long npages) {
    return syscall3(SYS_SHMEM_UNSHARE, target_pid, (sysarg_t)my_va, npages);
}

/* --- Winman registration ----------------------------------------------- */
long wm_register(void) { return syscall0(SYS_WM_REGISTER); }
long wm_pid(void)      { return syscall0(SYS_WM_PID); }

long tty_drain(char *buf, long max) {
    return syscall2(SYS_TTY_DRAIN, (sysarg_t)(uintptr_t)buf, max);
}

/* --- Diagnostics ------------------------------------------------------- */
long proc_list(struct proc_info *out, long max) {
    return syscall2(SYS_PROC_LIST, (sysarg_t)(uintptr_t)out, max);
}

long mem_stats(struct mem_stats *out) {
    return syscall1(SYS_MEM_STATS, (sysarg_t)(uintptr_t)out);
}

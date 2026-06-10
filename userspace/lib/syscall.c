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
    return syscall3(SYS_WRITE, fd, (long)(uintptr_t)buf, (long)(uintptr_t)n);
}

long read(int fd, void *buf, size_t n) {
    return syscall3(SYS_READ, fd, (long)(uintptr_t)buf, (long)(uintptr_t)n);
}

long open(const char *path, int flags) {
    return syscall2(SYS_OPEN, (long)(uintptr_t)path, flags);
}

long close(int fd) {
    return syscall1(SYS_CLOSE, fd);
}

long lseek(int fd, long off, int whence) {
    return syscall3(SYS_LSEEK, fd, off, whence);
}

/* Anonymous memory allocator. Returns 0 on failure, page-aligned va on
 * success — kernel decides the layout. */
void *mmap(size_t len) {
    long rc = syscall1(SYS_MMAP, (long)len);
    return rc < 0 ? 0 : (void*)(uintptr_t)rc;
}

long readdir(unsigned *index, char *buf, size_t n) {
    return syscall3(SYS_READDIR, (long)(uintptr_t)index, (long)(uintptr_t)buf, (long)n);
}

long unlink(const char *path) {
    return syscall1(SYS_UNLINK, (long)(uintptr_t)path);
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
    return syscall1(SYS_MSG_GET, (long)(uintptr_t)out);
}

long msg_peek(struct msg *out) {
    return syscall1(SYS_MSG_PEEK, (long)(uintptr_t)out);
}

long mouse_pos(int32_t *x, int32_t *y, uint8_t *buttons) {
    return syscall3(SYS_MOUSE_POS, (long)(uintptr_t)x, (long)(uintptr_t)y, (long)(uintptr_t)buttons);
}

/* --- Framebuffer -------------------------------------------------------- */
long fb_info(struct fb_info *out) {
    return syscall1(SYS_FB_INFO, (long)(uintptr_t)out);
}

void *fb_map(void) {
    return (void*)(uintptr_t)syscall0(SYS_FB_MAP);
}

long fb_damage(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    return syscall4(SYS_FB_DAMAGE, (long)x, (long)y, (long)w, (long)h);
}

long kbd_poll(int *pressed, uint16_t *key) {
    return syscall2(SYS_KBD_POLL, (long)(uintptr_t)pressed, (long)(uintptr_t)key);
}

long get_ticks(void) {
    return syscall0(SYS_GET_TICKS);
}

/* --- Child processes ---------------------------------------------------- */
/* Synchronous: blocks the caller until the child exits. Returns the
 * child's exit code (or -1 on failure to launch). */
long exec(const char *path, char *const argv[]) {
    return syscall2(SYS_EXEC, (long)(uintptr_t)path, (long)(uintptr_t)argv);
}

/* Fire-and-forget: returns the child's pid (or -1) without waiting.
 * Used by the shell for windowed apps so the prompt stays interactive. */
long spawn(const char *path, char *const argv[]) {
    return syscall2(SYS_SPAWN, (long)(uintptr_t)path, (long)(uintptr_t)argv);
}

long sys_shutdown(int time, const char *reason) {
    return syscall2(SYS_SHUTDOWN, time, (long)(uintptr_t)reason);
}

long sys_reboot(int time) {
    return syscall1(SYS_REBOOT, time);
}

/* --- Console (TTY) ------------------------------------------------------ */
long con_write(const char *buf, size_t n) {
    return syscall2(SYS_CON_WRITE, (long)(uintptr_t)buf, (long)(uintptr_t)n);
}

long con_clear(void) {
    return syscall0(SYS_CON_CLEAR);
}

long con_push(void) {
    return syscall0(SYS_CON_PUSH);
}

long con_pop(void) {
    return syscall0(SYS_CON_POP);
}

long sleep_ticks(unsigned long n) {
    return syscall1(SYS_SLEEP_TICKS, (long)n);
}

long get_pid(void) {
    return syscall0(SYS_GET_PID);
}

/* --- IPC and shared memory --------------------------------------------- */
long ipc_send(int target_pid, const struct ipc_msg *m) {
    return syscall2(SYS_IPC_SEND, target_pid, (long)(uintptr_t)m);
}

long ipc_recv(struct ipc_msg *out) {
    return syscall1(SYS_IPC_RECV, (long)(uintptr_t)out);
}

/* Map a contiguous range of pages from `my_va` into the target's address
 * space. Kernel chooses the target va and writes it back to out_target_va. */
long shmem_share(int target_pid, uint64_t my_va, long npages,
                 uint64_t *out_target_va) {
    return syscall4(SYS_SHMEM_SHARE, target_pid, (long)my_va, npages,
                    (long)(uintptr_t)out_target_va);
}

/* --- Winman registration ----------------------------------------------- */
long wm_register(void) { return syscall0(SYS_WM_REGISTER); }
long wm_pid(void)      { return syscall0(SYS_WM_PID); }

long tty_drain(char *buf, long max) {
    return syscall2(SYS_TTY_DRAIN, (long)(uintptr_t)buf, max);
}

/* --- Diagnostics ------------------------------------------------------- */
long proc_list(struct proc_info *out, long max) {
    return syscall2(SYS_PROC_LIST, (long)(uintptr_t)out, max);
}

long mem_stats(struct mem_stats *out) {
    return syscall1(SYS_MEM_STATS, (long)(uintptr_t)out);
}

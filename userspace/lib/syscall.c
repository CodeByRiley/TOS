/* userspace/lib/syscall.c , typed C wrappers around syscallN().
 *
 * Each function loads the syscall number plus the right argument
 * combination, casts pointers through uintptr_t so calling conventions
 * stay clean, and returns the kernel's long result unchanged. Anything
 * that isn't a syscall (winman IPC helpers, etc.) lives elsewhere , see
 * lib/wm.c.
 */
#include <lib/syscall.h>

long stat_raw(const char *path, struct stat_user *out) {
    return syscall2(SYS_STAT_RAW, (sysarg_t)(uintptr_t)path, (sysarg_t)(uintptr_t)out);
}

long fstat_raw(int fd, struct stat_user *out) {
    return syscall2(SYS_FSTAT_RAW, fd, (sysarg_t)(uintptr_t)out);
}

long readdir_path(const char *path, unsigned *index, char *buf, size_t n) {
    return syscall4(SYS_READDIR_PATH, (sysarg_t)(uintptr_t)path,
                    (sysarg_t)(uintptr_t)index, (sysarg_t)(uintptr_t)buf, (sysarg_t)n);
}

long mkdir_path(const char *path) {
    return syscall1(SYS_MKDIR, (sysarg_t)(uintptr_t)path);
}

long rmdir_path(const char *path) {
    return syscall1(SYS_RMDIR, (sysarg_t)(uintptr_t)path);
}

long yield(void) {
    return syscall0(SYS_YIELD);
}

/* Input event ring */
long msg_get(struct msg *out) {
    return syscall1(SYS_MSG_GET, (sysarg_t)(uintptr_t)out);
}

long msg_peek(struct msg *out) {
    return syscall1(SYS_MSG_PEEK, (sysarg_t)(uintptr_t)out);
}

long mouse_pos(int32_t *x, int32_t *y, uint8_t *buttons) {
    return syscall3(SYS_MOUSE_POS, (sysarg_t)(uintptr_t)x, (sysarg_t)(uintptr_t)y, (sysarg_t)(uintptr_t)buttons);
}

/* Framebuffer */
long fb_info(struct fb_info *out) {
    return syscall1(SYS_FB_INFO, (sysarg_t)(uintptr_t)out);
}

void *fb_map(void) {
    return (void*)(uintptr_t)syscall0(SYS_FB_MAP);
}

long fb_damage(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    return syscall4(SYS_FB_DAMAGE, x, y, w, h);
}

long fb_present(const void *pixels, uint32_t pitch,
                const struct fb_rect *rects, uint32_t rect_count) {
    return syscall4(SYS_FB_PRESENT, (sysarg_t)(uintptr_t)pixels, pitch,
                    (sysarg_t)(uintptr_t)rects, rect_count);
}

long fb_register(const void *pixels, uint32_t pitch) {
    return syscall2(SYS_FB_REGISTER, (sysarg_t)(uintptr_t)pixels, pitch);
}

long fb_unregister(void) {
    return syscall0(SYS_FB_UNREGISTER);
}

long kbd_poll(int *pressed, uint16_t *key) {
    return syscall2(SYS_KBD_POLL, (sysarg_t)(uintptr_t)pressed, (sysarg_t)(uintptr_t)key);
}

long get_ticks(void) {
    return syscall0(SYS_GET_TICKS);
}

/* Child processes */
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

long sys_shutdown(int time, const char *reason) {
    return syscall2(SYS_SHUTDOWN, time, (sysarg_t)(uintptr_t)reason);
}

long sys_reboot(int time) {
    return syscall1(SYS_REBOOT, time);
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

void tty_inject(char c) {
    syscall1(SYS_TTY_INJECT, (long)c);
}

long tty_read_input(char *buf, unsigned long max) {
    return syscall2(SYS_TTY_READ_INPUT, (sysarg_t)(uintptr_t)buf,
                    (sysarg_t)max);
}

long get_pid(void) {
    return syscall0(SYS_GET_PID);
}

/* Raw SYS_CLOCK_GETTIME. include/time.h's clock_gettime() wraps this; the
 * struct is the kernel's linux_timespec, declared void * here so syscall.h
 * does not have to know about <time.h>. */
long sys_clock_gettime(int clock_id, void *ts) {
    return syscall2(SYS_CLOCK_GETTIME, (sysarg_t)clock_id,
                    (sysarg_t)(uintptr_t)ts);
}

long audio_status(struct audio_status *out) {
    return syscall1(SYS_AUDIO_STATUS, (sysarg_t)(uintptr_t)out);
}

long audio_drain(void) {
    return syscall0(SYS_AUDIO_DRAIN);
}

long audio_set_volume(int percent) {
    return syscall1(SYS_AUDIO_SET_VOLUME, percent);
}

long audio_pause(void) {
    return syscall0(SYS_AUDIO_PAUSE);
}

long audio_resume(void) {
    return syscall0(SYS_AUDIO_RESUME);
}

/* IPC and shared memory */
long ipc_send(int target_pid, const struct ipc_msg *m) {
    return syscall2(SYS_IPC_SEND, target_pid, (sysarg_t)(uintptr_t)m);
}

long ipc_recv(struct ipc_msg *out) {
    return syscall1(SYS_IPC_RECV, (sysarg_t)(uintptr_t)out);
}

/* Map a contiguous range of pages from `in_va` into the target's address
 * space. Kernel chooses the target va and writes it back to out_target_va. */
long shmem_share(int target_pid, uint64_t in_va, long npages,
                 uint64_t *out_target_va) {
    return syscall4(SYS_SHMEM_SHARE, target_pid, (sysarg_t)in_va, npages,
                    (sysarg_t)(uintptr_t)out_target_va);
}

long shmem_unshare(int target_pid, uint64_t in_va, long npages) {
    return syscall3(SYS_SHMEM_UNSHARE, target_pid, (sysarg_t)in_va, npages);
}

/* Winman registration */
long wm_register(void) { return syscall0(SYS_WM_REGISTER); }
long wm_pid(void)      { return syscall0(SYS_WM_PID); }

long tty_drain(char *buf, long max) {
    return syscall2(SYS_TTY_DRAIN, (sysarg_t)(uintptr_t)buf, max);
}

/* Diagnostics */
long proc_list(struct proc_info *out, long max) {
    return syscall2(SYS_PROC_LIST, (sysarg_t)(uintptr_t)out, max);
}

long mem_stats(struct mem_stats *out) {
    return syscall1(SYS_MEM_STATS, (sysarg_t)(uintptr_t)out);
}

long net_stats(struct net_stats *out) {
    return syscall1(SYS_NET_STATS, (sysarg_t)(uintptr_t)out);
}

long net_capture(uint64_t *cursor, struct net_frame *out, long max) {
    return syscall3(SYS_NET_CAPTURE, (sysarg_t)(uintptr_t)cursor,
                    (sysarg_t)(uintptr_t)out, (sysarg_t)max);
}

long net_ping(struct net_ping *req) {
    return syscall1(SYS_NET_PING, (sysarg_t)(uintptr_t)req);
}

/* Threading */
long thread_create(void *(*entry)(void *), void *stack, void *arg) {
    return syscall3(SYS_THREAD_CREATE,
                    (sysarg_t)(uintptr_t)entry,
                    (sysarg_t)(uintptr_t)stack,
                    (sysarg_t)(uintptr_t)arg);
}

long thread_join(long tid) {
    return syscall1(SYS_THREAD_JOIN, tid);
}

long futex_wait(uint32_t *addr, uint32_t expected) {
    return syscall2(SYS_FUTEX_WAIT, (sysarg_t)(uintptr_t)addr, expected);
}

long futex_wake(uint32_t *addr) {
    return syscall1(SYS_FUTEX_WAKE, (sysarg_t)(uintptr_t)addr);
}

/* Console (TTY) */
long con_write(const char *buf, size_t n) {
    return syscall2(SYS_CON_WRITE, (sysarg_t)(uintptr_t)buf, (sysarg_t)(uintptr_t)n);
}
/* PCM audio */
long audio_open(uint32_t sample_rate, uint32_t channels, uint32_t format) {
    return syscall3(SYS_AUDIO_OPEN, sample_rate, channels, format);
}
long audio_write(const void *pcm, size_t bytes) {
    return syscall2(SYS_AUDIO_WRITE, (sysarg_t)(uintptr_t)pcm,
                    (sysarg_t)bytes);
}
long audio_close(void) {
    return syscall0(SYS_AUDIO_CLOSE);
}
long thread_exit() {
    return syscall0(SYS_THREAD_EXIT);
}

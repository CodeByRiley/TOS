#include "syscall.h"

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

void exit(int code) {
    syscall1(SYS_EXIT, code);
    for (;;);   /* unreachable but compiler doesn't know */
}

long yield(void) {
    return syscall0(SYS_YIELD);
}

long msg_get(struct msg *out) {
    return syscall1(SYS_MSG_GET, (long)(uintptr_t)out);
}

long msg_peek(struct msg *out) {
    return syscall1(SYS_MSG_PEEK, (long)(uintptr_t)out);
}

long mouse_pos(int32_t *x, int32_t *y, uint8_t *buttons) {
    return syscall3(SYS_MOUSE_POS, (long)(uintptr_t)x, (long)(uintptr_t)y, (long)(uintptr_t)buttons);
}

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

long exec(const char *path, char *const argv[]) {
    /* runs child synchronously; returns child's exit code (or -1 on fail) */
    return syscall2(SYS_EXEC, (long)(uintptr_t)path, (long)(uintptr_t)argv);
}

long sys_shutdown(int time, const char *reason) {
    return syscall2(SYS_SHUTDOWN, time, (long)(uintptr_t)reason);
}

long sys_reboot(int time) {
    return syscall1(SYS_REBOOT, time);
}

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

long ipc_send(int target_pid, const struct ipc_msg *m) {
    return syscall2(SYS_IPC_SEND, target_pid, (long)(uintptr_t)m);
}

long ipc_recv(struct ipc_msg *out) {
    return syscall1(SYS_IPC_RECV, (long)(uintptr_t)out);
}

long shmem_share(int target_pid, uint64_t my_va, long npages,
                 uint64_t *out_target_va) {
    return syscall4(SYS_SHMEM_SHARE, target_pid, (long)my_va, npages,
                    (long)(uintptr_t)out_target_va);
}

long wm_register(void) { return syscall0(SYS_WM_REGISTER); }
long wm_pid(void)      { return syscall0(SYS_WM_PID); }

long tty_drain(char *buf, long max) {
    return syscall2(SYS_TTY_DRAIN, (long)(uintptr_t)buf, max);
}

long proc_list(struct proc_info *out, long max) {
    return syscall2(SYS_PROC_LIST, (long)(uintptr_t)out, max);
}

long mem_stats(struct mem_stats *out) {
    return syscall1(SYS_MEM_STATS, (long)(uintptr_t)out);
}

/* --- winman client wrappers ---------------------------------------- */

extern size_t strlen(const char *);
extern void  *memset(void *, int, size_t);
extern void  *memcpy(void *, const void *, size_t);

/* Spin until we get an ipc_msg of the requested type. Other messages
 * are dropped — clients aren't expected to receive unrelated traffic
 * mid-handshake. Bounded so we don't hang forever if winman dies. */
static int wait_for(uint32_t type, struct ipc_msg *out) {
    for (int spin = 0; spin < 100000; spin++) {
        if (ipc_recv(out)) {
            if (out->type == type) return 0;
            /* ignore unrelated messages during handshake */
        } else {
            sleep_ticks(1);
        }
    }
    return -1;
}

int winman_create(int w, int h, const char *title,
                  struct winman_window *out) {
    if (!out) return -1;
    long wpid = wm_pid();
    if (wpid <= 0) return -1;

    struct ipc_msg req;
    memset(&req, 0, sizeof(req));
    req.type = IPC_WM_CREATE_REQ;
    req.a = w;
    req.b = h;
    if (title) {
        size_t n = strlen(title);
        if (n > sizeof(req.str) - 1) n = sizeof(req.str) - 1;
        memcpy(req.str, title, n);
        req.str[n] = 0;
    }
    if (ipc_send((int)wpid, &req) != 0) return -1;

    struct ipc_msg resp;
    if (wait_for(IPC_WM_CREATE_RESP, &resp) != 0) return -1;
    if (resp.a < 0) return -1;

    out->handle     = resp.a;
    out->surface_va = resp.va;
    out->pitch      = resp.pitch;
    out->w          = w;
    out->h          = h;
    return 0;
}

int winman_destroy(int handle) {
    long wpid = wm_pid();
    if (wpid <= 0) return -1;
    struct ipc_msg req;
    memset(&req, 0, sizeof(req));
    req.type = IPC_WM_DESTROY_REQ;
    req.a = handle;
    return (int)ipc_send((int)wpid, &req);
}

int winman_invalidate(int handle) {
    long wpid = wm_pid();
    if (wpid <= 0) return -1;
    struct ipc_msg req;
    memset(&req, 0, sizeof(req));
    req.type = IPC_WM_INVALIDATE_REQ;
    req.a = handle;
    return (int)ipc_send((int)wpid, &req);
}

int winman_set_title(int handle, const char *title) {
    long wpid = wm_pid();
    if (wpid <= 0) return -1;
    struct ipc_msg req;
    memset(&req, 0, sizeof(req));
    req.type = IPC_WM_SET_TITLE_REQ;
    req.a = handle;
    if (title) {
        size_t n = strlen(title);
        if (n > sizeof(req.str) - 1) n = sizeof(req.str) - 1;
        memcpy(req.str, title, n);
        req.str[n] = 0;
    }
    return (int)ipc_send((int)wpid, &req);
}

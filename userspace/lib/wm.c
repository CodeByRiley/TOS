/*
 * libwm — userspace client implementation of the window manager protocol.
 *
 * Thin synchronous wrappers around ipc_send + ipc_recv. The server lives
 * in userspace/bin/winman/winman.c and speaks the same wire protocol
 * declared in wm.h.
 *
 * All "winman pid" lookups go through wm_pid() (a syscall): the WM is
 * allowed to crash and respawn, so caching the pid would be wrong.
 */

#include "wm.h"

extern size_t strlen(const char *);
extern void  *memset(void *, int, size_t);
extern void  *memcpy(void *, const void *, size_t);

/* Bounded spin-recv for handshake replies. Drops unrelated messages so a
 * stray IPC_WM_INPUT during create doesn't trip the wait. Bounded so that
 * a dead winman doesn't hang the client forever. */
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

/* Copy `src` into `dst` (a fixed-size on-wire string field), null-terminate
 * even on truncation. Used by create + set_title. */
static void copy_str(char *dst, size_t cap, const char *src) {
    if (cap == 0) return;
    if (!src) { dst[0] = 0; return; }
    size_t n = strlen(src);
    if (n > cap - 1) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

int wm_window_create(int w, int h, const char *title,
                     struct wm_window *out) {
    if (!out) return -1;
    long wpid = wm_pid();
    if (wpid <= 0) return -1;

    struct ipc_msg req;
    memset(&req, 0, sizeof(req));
    req.type = IPC_WM_CREATE_REQ;
    req.a    = w;
    req.b    = h;
    copy_str(req.str, sizeof(req.str), title);
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

int wm_window_destroy(int handle) {
    long wpid = wm_pid();
    if (wpid <= 0) return -1;
    struct ipc_msg req;
    memset(&req, 0, sizeof(req));
    req.type = IPC_WM_DESTROY_REQ;
    req.a    = handle;
    return (int)ipc_send((int)wpid, &req);
}

int wm_window_invalidate(int handle) {
    long wpid = wm_pid();
    if (wpid <= 0) return -1;
    struct ipc_msg req;
    memset(&req, 0, sizeof(req));
    req.type = IPC_WM_INVALIDATE_REQ;
    req.a    = handle;
    return (int)ipc_send((int)wpid, &req);
}

int wm_window_set_title(int handle, const char *title) {
    long wpid = wm_pid();
    if (wpid <= 0) return -1;
    struct ipc_msg req;
    memset(&req, 0, sizeof(req));
    req.type = IPC_WM_SET_TITLE_REQ;
    req.a    = handle;
    copy_str(req.str, sizeof(req.str), title);
    return (int)ipc_send((int)wpid, &req);
}

int wm_poll_event(struct wm_event *out) {
    if (!out) return 0;
    struct ipc_msg m;
    if (!ipc_recv(&m)) return 0;

    switch (m.type) {
    case IPC_WM_INPUT:
        /* winman packs (msg_type, param, x, y) into (a, b, c, d).
         * MSG_* numbers (1..5) are intentionally aligned with WM_EV_*. */
        memset(out, 0, sizeof(*out));
        out->type  = m.a;
        out->param = m.b;
        out->x     = m.c;
        out->y     = m.d;
        return 1;

    case IPC_WM_RESIZE_NOTIFY:
        memset(out, 0, sizeof(*out));
        out->type       = WM_EV_RESIZE;
        out->w          = m.b;
        out->h          = m.c;
        out->x          = m.b;          /* convenience alias */
        out->y          = m.c;
        out->surface_va = m.va;
        out->pitch      = m.pitch;
        return 1;

    default:
        /* Non-WM IPC: drop on the floor. Apps that need to mix custom IPC
         * with WM events should poll ipc_recv themselves and dispatch on
         * msg type before falling through to libwm. */
        memset(out, 0, sizeof(*out));
        out->type = WM_EV_NONE;
        return 0;
    }
}

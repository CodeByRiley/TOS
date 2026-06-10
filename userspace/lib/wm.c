/* userspace/lib/wm.c — libwm: client-side IPC for windows.
 *
 * Thin synchronous wrappers around ipc_send + ipc_recv. The server lives
 * in userspace/bin/winman/winman.c and speaks the same wire protocol
 * declared in wm.h.
 *
 * The "winman pid" is looked up via wm_pid() on every call — never
 * cached, because winman is allowed to crash and respawn.
 */
#include "wm.h"

extern size_t strlen(const char *);
extern void  *memset(void *, int, size_t);
extern void  *memcpy(void *, const void *, size_t);

/* Bounded spin-recv used for handshake replies. Drops unrelated messages
 * so a stray IPC_WM_INPUT during create doesn't trip the wait. Caps the
 * spin so a dead winman doesn't hang the client forever. */
static int wait_for(uint32_t type, struct ipc_msg *out) {
    for (int spin = 0; spin < 100000; spin++) {
        if (ipc_recv(out)) {
            if (out->type == type) return 0;
        } else {
            sleep_ticks(1);
        }
    }
    return -1;
}

/* Copy `src` into a fixed-size on-wire string field, NUL-terminating even
 * when truncated. Used by create + set_title. */
static void copy_str(char *dst, size_t cap, const char *src) {
    if (cap == 0) return;
    if (!src) { dst[0] = 0; return; }
    size_t n = strlen(src);
    if (n > cap - 1) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* Synchronous CREATE handshake: send req, wait for CREATE_RESP, copy the
 * server's chosen handle + surface mapping into *out. */
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
    printf("wm_window_create: sent create req, waiting for response...\n");
    struct ipc_msg resp;
    if (wait_for(IPC_WM_CREATE_RESP, &resp) != 0) return -1;
    if (resp.a < 0) return -1;
    printf("wm_window_create: sizeof(wm_window)=%zu bytes\n", sizeof(struct wm_window));
    printf("wm_window_create: sizeof(wm_event)=%zu bytes\n", sizeof(struct wm_event));
    out->handle     = resp.a;
    out->surface_va = resp.va;
    out->pitch      = resp.pitch;
    out->w          = w;
    out->h          = h;
    return 0;
}

/* Fire-and-forget DESTROY. */
int wm_window_destroy(int handle) {
    long wpid = wm_pid();
    if (wpid <= 0) return -1;
    struct ipc_msg req;
    memset(&req, 0, sizeof(req));
    req.type = IPC_WM_DESTROY_REQ;
    req.a    = handle;
    return (int)ipc_send((int)wpid, &req);
}

/* Mark the window dirty so winman recomposites. */
int wm_window_invalidate(int handle) {
    long wpid = wm_pid();
    if (wpid <= 0) return -1;
    struct ipc_msg req;
    memset(&req, 0, sizeof(req));
    req.type = IPC_WM_INVALIDATE_REQ;
    req.a    = handle;
    return (int)ipc_send((int)wpid, &req);
}

/* Update chrome title. The IPC field is fixed-size; longer titles get
 * silently truncated to 47 bytes by copy_str. */
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

/* Translate one queued IPC message into a wm_event. Returns 1 if filled
 * with a real event, 0 if the queue was empty or the message was non-WM. */
int wm_poll_event(struct wm_event *out) {
    if (!out) return 0;
    struct ipc_msg m;
    if (!ipc_recv(&m)) return 0;

    switch (m.type) {
    case IPC_WM_INPUT:
        /* winman packs (msg_type, param, x, y) into (a, b, c, d).
         * MSG_* codes (1..5) are intentionally aligned with WM_EV_*. */
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
        /* Non-WM IPC: drop it. Apps that need to mix custom IPC with WM
         * events should call ipc_recv themselves and dispatch first. */
        memset(out, 0, sizeof(*out));
        out->type = WM_EV_NONE;
        return 0;
    }
}

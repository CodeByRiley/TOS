/* userspace/lib/wm.c , libwm: client-side IPC for windows.
 *
 * Thin synchronous wrappers around ipc_send + ipc_recv. The server lives
 * in userspace/bin/winman/winman.c and speaks the same wire protocol
 * declared in wm.h.
 *
 * The "winman pid" is looked up via wm_pid() on every call , never
 * cached, because winman is allowed to crash and respawn.
 */
#include "wm.h"
#include "event.h"

extern size_t strlen(const char *);
extern void  *memset(void *, int, size_t);
extern void  *memcpy(void *, const void *, size_t);

/* Bounded reply wait. libevent preserves unrelated IPC instead of dropping
 * it, which lets a windowed client use its own protocol at the same time. */
static int wait_for(uint32_t type, int sender_pid, struct ipc_msg *out) {
    return event_wait_type(out, type, sender_pid, 100000u) == EVENT_READY
               ? 0
               : -1;
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
    return wm_window_create_ex(w, h, title, 0, out);
}

int wm_window_create_ex(int w, int h, const char *title, uint32_t flags,
                        struct wm_window *out) {
    if (!out) return -1;
    long wpid = wm_pid();
    if (wpid <= 0) return -1;

    struct ipc_msg req;
    memset(&req, 0, sizeof(req));
    req.type  = IPC_WM_CREATE_REQ;
    req.a     = w;
    req.b     = h;
    req.flags = flags;
    copy_str(req.str, sizeof(req.str), title);
    if (ipc_send((int)wpid, &req) != 0) return -1;
    struct ipc_msg resp;
    if (wait_for(IPC_WM_CREATE_RESP, (int)wpid, &resp) != 0) return -1;
    if (resp.a < 0) return -1;
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

/* Update the status strip. Windows created without WM_CREATE_STATUSBAR
 * have nowhere to put this, and winman drops it. */
int wm_window_set_status(int handle, const char *text) {
    long wpid = wm_pid();
    if (wpid <= 0) return -1;
    struct ipc_msg req;
    memset(&req, 0, sizeof(req));
    req.type = IPC_WM_SET_STATUS_REQ;
    req.a    = handle;
    copy_str(req.str, sizeof(req.str), text);
    return (int)ipc_send((int)wpid, &req);
}

/* Blocking modal prompt. The wait is deliberately unbounded compared with
 * wait_for(): a dialog sits open until a human answers it, so the bounded
 * handshake spin used for create/destroy would time out mid-question. A
 * dead winman is detected by re-checking wm_pid() rather than by counting
 * spins. */
int wm_prompt(int handle, int kind, const char *message, char *out,
              size_t cap) {
    if (out && cap) out[0] = 0;

    long wpid = wm_pid();
    if (wpid <= 0) return WM_PROMPT_CANCEL;

    struct ipc_msg req;
    memset(&req, 0, sizeof(req));
    req.type = IPC_WM_PROMPT_REQ;
    req.a    = handle;
    req.b    = kind;
    copy_str(req.str, sizeof(req.str), message);
    if (ipc_send((int)wpid, &req) != 0) return WM_PROMPT_CANCEL;

    for (;;) {
        struct ipc_msg resp;
        int result = event_poll_type(&resp, IPC_WM_PROMPT_RESP, (int)wpid);
        if (result == EVENT_READY) {
            if (out && cap) copy_str(out, cap, resp.str);
            return resp.a;
        }
        if (result < 0) return WM_PROMPT_CANCEL;
        if (wm_pid() <= 0) return WM_PROMPT_CANCEL;
        sleep_ticks(1);
    }
}

static int is_wm_event(const struct ipc_msg *message, void *context) {
    int sender_pid = *(const int *)context;
    return (int)message->from_pid == sender_pid &&
           (message->type == IPC_WM_INPUT ||
            message->type == IPC_WM_RESIZE_NOTIFY);
}

/* Translate one queued IPC message into a wm_event. Returns 1 if filled
 * with a real event, 0 if the queue was empty or the message was non-WM. */
int wm_poll_event(struct wm_event *out) {
    if (!out) return 0;
    long wpid = wm_pid();
    if (wpid <= 0) return 0;

    struct ipc_msg m;
    int sender_pid = (int)wpid;
    if (event_poll_matching(&m, is_wm_event, &sender_pid) != EVENT_READY)
        return 0;

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
        memset(out, 0, sizeof(*out));
        out->type = WM_EV_NONE;
        return 0;
    }
}

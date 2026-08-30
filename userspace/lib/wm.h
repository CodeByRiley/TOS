/* userspace/lib/wm.h , libwm: userspace window-manager client API.
 *
 * Apps include this header to talk to winman over IPC without knowing
 * the wire protocol. The functions wrap a synchronous request/reply
 * handshake built on ipc_send + ipc_recv (declared in syscall.h, which
 * this header pulls in).
 *
 * The server lives in userspace/bin/winman/winman.c and includes this
 * same header so wire types and `struct ipc_msg` field layout stay
 * single-sourced. The kernel msg dispatcher (kernel/msg/msg.h) only
 * routes opaque ipc_msg payloads , IPC_WM_* codes are user-defined.
 */
#ifndef USER_WM_H
#define USER_WM_H

#ifdef TOS_USE_MUSL
#include <stdio.h>
#else
#include <include/stdio.h>
#endif
#include <lib/syscall.h>
#include <stdint.h>

/* ---------------- Wire protocol ---------------------------------------- */
/* IPC message type codes. Must stay in sync with winman.c's pump_ipc()
 * switch. Values are inside the IPC_USER_FIRST range. */
#define IPC_WM_CREATE_REQ       0x100
#define IPC_WM_CREATE_RESP      0x101
#define IPC_WM_DESTROY_REQ      0x102
#define IPC_WM_INVALIDATE_REQ   0x103
#define IPC_WM_SET_TITLE_REQ    0x104
#define IPC_WM_SET_STATUS_REQ   0x105
#define IPC_WM_PROMPT_REQ       0x106
#define IPC_WM_PROMPT_RESP      0x107
#define IPC_WM_INPUT            0x110
#define IPC_WM_RESIZE_NOTIFY    0x111

/* Creation flags, passed in ipc_msg.flags on IPC_WM_CREATE_REQ. */
/* Give the window a status strip along the bottom of its frame. The client
 * area is unaffected , the frame grows to make room , so a window that asks
 * for one does not silently lose drawing space. */
#define WM_CREATE_STATUSBAR     0x1u

/* Prompt kinds for wm_prompt(). */
enum {
    WM_PROMPT_MESSAGE = 0, /* one OK button                        */
    WM_PROMPT_CONFIRM = 1, /* Yes / No / Cancel                    */
    WM_PROMPT_TEXT    = 2, /* single-line entry, Enter/Escape      */
};

/* Prompt results, returned in ipc_msg.a on IPC_WM_PROMPT_RESP. */
enum {
    WM_PROMPT_CANCEL = 0, /* Escape, Cancel, or the request failed */
    WM_PROMPT_OK     = 1, /* OK / Yes / Enter                      */
    WM_PROMPT_NO     = 2, /* No , confirm dialogs only             */
};

#define MAX_WINDOWS 64

/* ---------------- Window handle ---------------------------------------- */
/* Returned by wm_window_create. `surface_va` is the page-aligned base of
 * the shared BGRA pixel buffer mapped into the client's address space ,
 * write pixels there directly, then call wm_window_invalidate so winman
 * recomposites. `pitch` is the row stride in bytes (== w*4 today). */
struct wm_window {
    uint64_t surface_va;
    int      handle;
    uint32_t pitch;
    int      w, h;
};

/* ---------------- Event API -------------------------------------------- */
/* wm_poll_event() selects WM-flavored messages through lib/event. Non-WM
 * messages remain queued in libevent's userspace inbox, so windowed clients
 * can safely mix WM and custom IPC. Event codes mirror MSG_* from syscall.h
 * so apps see the same input vocabulary winman receives from the kernel. */
enum {
    WM_EV_NONE        = 0,
    WM_EV_KEY_DOWN    = 1,
    WM_EV_KEY_UP      = 2,
    WM_EV_MOUSE_MOVE  = 3,
    WM_EV_MOUSE_DOWN  = 4,
    WM_EV_MOUSE_UP    = 5,
    WM_EV_RESIZE      = 100,
    WM_EV_QUIT        = 101,
};

struct wm_event {
    int      type;       /* WM_EV_*                                       */
    int      param;      /* keycode or button mask                        */
    int      x, y;       /* mouse pos (input) or new client size (resize) */
    int      w, h;       /* new client size on WM_EV_RESIZE               */
    uint64_t surface_va; /* new shared-surface va on WM_EV_RESIZE         */
    uint32_t pitch;      /* new row stride on WM_EV_RESIZE                */
};

/* ---------------- Client API ------------------------------------------- */
/* All functions return 0 on success and -1 on failure (winman missing,
 * IPC failure, or server-side allocation failure). */

/* Create a `w` x `h` BGRA window. Blocks until winman replies. On success
 * `out` is filled in and the surface is mapped RW at out->surface_va. */
int  wm_window_create(int w, int h, const char *title, struct wm_window *out);

/* As wm_window_create, plus WM_CREATE_* flags. The plain form is this with
 * flags == 0. */
int  wm_window_create_ex(int w, int h, const char *title, uint32_t flags,
                         struct wm_window *out);

/* Set the status-strip text. No-op on a window created without
 * WM_CREATE_STATUSBAR. Truncated to 47 bytes like the title. */
int  wm_window_set_status(int handle, const char *text);

/* Put up a modal dialog owned by `handle` and block until the user answers.
 *
 * `kind` is WM_PROMPT_*. For WM_PROMPT_TEXT the reply is copied into `out`
 * (NUL-terminated, truncated to `cap`); pass NULL/0 for the other kinds.
 * Returns WM_PROMPT_OK, WM_PROMPT_NO, or WM_PROMPT_CANCEL , a dead winman
 * or a failed request reads as WM_PROMPT_CANCEL, so callers only have to
 * handle "the user did not say yes".
 *
 * Winman draws and drives the dialog; the calling app neither renders it
 * nor sees the keystrokes that go into it. */
int  wm_prompt(int handle, int kind, const char *message, char *out,
               size_t cap);

/* Tear down a window. After this call surface_va becomes invalid. */
int  wm_window_destroy(int handle);

/* Tell winman the window's pixels changed and need recompositing. Today
 * this is whole-window damage; future versions will accept a rect. */
int  wm_window_invalidate(int handle);

/* Replace the chrome title. Truncated to 47 bytes by the IPC message. */
int  wm_window_set_title(int handle, const char *title);

/* Non-blocking event poll. Returns 1 if `out` was filled, 0 otherwise.
 * Apps should call this every frame. */
int  wm_poll_event(struct wm_event *out);

#endif

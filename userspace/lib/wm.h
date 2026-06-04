#ifndef USER_WM_H
#define USER_WM_H

/*
 * libwm — userspace client API for the window manager (winman).
 *
 * Apps include this header to talk to winman over IPC without knowing the
 * wire protocol. The functions wrap a synchronous request/reply handshake
 * built on ipc_send + ipc_recv (declared in syscall.h, which this header
 * includes).
 *
 * Server-side counterparts live in userspace/bin/winman/winman.c, which
 * also includes this header so the IPC_WM_* type codes and `struct ipc_msg`
 * layout stay in one place.
 */

#include "syscall.h"
#include <stdint.h>

/* ===== Wire protocol =================================================== */
/* IPC message type codes. Must stay in sync with winman.c's switch in
 * pump_ipc() and the kernel msg dispatcher (src/intf/msg/msg.h). */
#define IPC_WM_CREATE_REQ       0x100
#define IPC_WM_CREATE_RESP      0x101
#define IPC_WM_DESTROY_REQ      0x102
#define IPC_WM_INVALIDATE_REQ   0x103
#define IPC_WM_SET_TITLE_REQ    0x104
#define IPC_WM_INPUT            0x110
#define IPC_WM_RESIZE_NOTIFY    0x111

/* ===== Window handle ==================================================== */
/* Returned by wm_window_create. `surface_va` is the page-aligned base of
 * the shared pixel buffer mapped into the client's address space; the
 * client writes BGRA pixels there directly and then calls wm_window_invalidate
 * to ask winman to recomposite. `pitch` is row stride in bytes (== w*4). */
struct wm_window {
    int      handle;
    uint64_t surface_va;
    uint32_t pitch;
    int      w, h;
};

/* ===== Event API ======================================================== */
/* wm_poll_event() drains ipc_recv looking for WM-flavored messages
 * (IPC_WM_INPUT, IPC_WM_RESIZE_NOTIFY). Non-WM messages are pushed back
 * by being dropped — clients that mix WM and non-WM IPC should call
 * ipc_recv directly and dispatch themselves.
 *
 * Event types mirror the MSG_* codes from syscall.h so a focused client
 * sees the same input vocabulary winman sees from the kernel msg ring. */
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

/* ===== Client API ======================================================= */
/* All functions return 0 on success, -1 on failure (winman not present,
 * IPC failure, allocation failure inside winman). */

/* Create a `w` x `h` BGRA window with the given title. Blocks until winman
 * replies. On success, `out` is filled in and the surface is mapped read/
 * write at out->surface_va. */
int  wm_window_create(int w, int h, const char *title, struct wm_window *out);

/* Tear down a window. After this call the surface_va becomes invalid. */
int  wm_window_destroy(int handle);

/* Tell winman the window's surface has new pixels and needs recompositing.
 * Today this is whole-window damage; future work will accept a rect. */
int  wm_window_invalidate(int handle);

/* Update the chrome title text. Truncated to 47 bytes by the IPC msg. */
int  wm_window_set_title(int handle, const char *title);

/* Non-blocking event poll. Returns 1 if an event was filled, 0 otherwise.
 * Should be called every frame in the app's main loop. */
int  wm_poll_event(struct wm_event *out);

#endif

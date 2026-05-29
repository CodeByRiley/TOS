#ifndef MSG_H
#define MSG_H

#include <stdint.h>

/* Win3-style event message. Producers (IRQ handlers, timers) post; consumers
 * (the foreground process via SYS_MSG_GET) pop. Each task owns its own ring
 * so that the kernel can route input to a single "input owner" (typically
 * the userspace winman) without other processes racing on a global queue. */

#define MSG_NONE        0
#define MSG_KEY_DOWN    1
#define MSG_KEY_UP      2
#define MSG_MOUSE_MOVE  3
#define MSG_MOUSE_DOWN  4
#define MSG_MOUSE_UP    5
#define MSG_TIMER       6
#define MSG_QUIT        7

struct msg {
    uint16_t type;     /* MSG_*                                          */
    uint16_t param;    /* KEY_* keycode for key events; button mask for
                          mouse-button events; unused for moves          */
    int16_t  x;        /* mouse abs cursor X (or relative dx for KEY)    */
    int16_t  y;        /* mouse abs cursor Y                             */
    uint32_t when;     /* PIT ticks at post time                         */
};

/* Larger inter-process control message. Used for winman <-> client
 * protocol, kernel notifications, etc. Sender and recipient identified
 * by pid; kernel deep-copies the struct into the recipient's per-task
 * ring on SYS_IPC_SEND. */

#define IPC_WM_CREATE_REQ      0x100   /* client -> wm: a=w b=h, str=title    */
#define IPC_WM_CREATE_RESP     0x101   /* wm -> client: a=handle va=surface_va pitch=pitch (a<0 on fail) */
#define IPC_WM_DESTROY_REQ     0x102   /* client -> wm: a=handle             */
#define IPC_WM_INVALIDATE_REQ  0x103   /* client -> wm: a=handle             */
#define IPC_WM_SET_TITLE_REQ   0x104   /* client -> wm: a=handle, str=title  */
#define IPC_WM_INPUT           0x110   /* wm -> client: forwarded input (a=msg_type, b=param, c=x, d=y) */
#define IPC_PEER_EXITED        0x180   /* kernel -> any: a=pid_that_exited   */
#define IPC_USER_FIRST         0x200   /* userspace-defined ids start here   */

struct ipc_msg {
    uint32_t type;       /* IPC_*                                      */
    uint32_t from_pid;   /* set by kernel on send                      */
    int32_t  a, b, c, d; /* generic integer payload                    */
    uint64_t va;         /* shared-memory va or 64-bit payload         */
    uint32_t pitch;      /* surface pitch (bytes per row) on responses */
    uint32_t flags;
    char     str[48];    /* title / name / arbitrary short string      */
};

void msg_init(void);

/* Post an input event onto the input-owner's ring. Called from IRQ
 * contexts — must not block. Drops the event silently if no input owner
 * is set or the recipient's ring is full. */
void msg_post(const struct msg *m);

/* Post directly to a specific pid's input ring. */
void msg_post_to(int pid, const struct msg *m);

/* Pop one event from the calling task's ring. Returns 1 on success,
 * 0 if ring empty / task has no ring. */
int  msg_get(struct msg *out);
int  msg_peek(struct msg *out);

/* Post an IPC message to `target_pid`. Kernel fills out->from_pid. */
int  ipc_send(int target_pid, const struct ipc_msg *m, int from_pid);

/* Pop one IPC message from the calling task's ring. */
int  ipc_recv(struct ipc_msg *out);

/* Set / get the input owner. Returns 0 on success, -1 if already owned. */
int  msg_input_owner_register(int pid);
void msg_input_owner_force(int pid);
void msg_input_owner_clear(int pid);
int  msg_input_owner(void);

#endif

/* kernel/msg/msg.h , per-task message rings (input + IPC).
 *
 * Two ring layers share this header:
 *   - struct msg     : Win3-style input event ring. IRQ handlers + timers
 *                       post; the foreground process pops via SYS_MSG_GET.
 *                       The kernel routes input to a single "input owner"
 *                       (typically userspace winman) so other processes
 *                       don't race on a global queue.
 *   - struct ipc_msg : Larger cross-process control messages. Used for
 *                       the winman <-> client protocol and for kernel-
 *                       originated notifications (peer-exited, etc.).
 *
 * Both layouts live in arch/syscall_abi.h because the kernel and userspace
 * copy them verbatim across the syscall boundary.
 *
 * Implementation: kernel/msg/msg.c.
 */
#ifndef MSG_H
#define MSG_H

#include <stddef.h>
#include <stdint.h>
#include <utilities/types.h>
#include <arch/syscall_abi.h>

void msg_init(void);

/* Post an input event onto the input-owner's ring. Called from IRQ
 * contexts , must not block. Drops the event silently if no input owner
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

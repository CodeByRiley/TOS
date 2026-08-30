/* Process-local IPC mailbox with filtering and lossless deferred messages.
 * This API is intentionally single-consumer and is not thread-safe. Once a
 * program uses libevent (directly or through libwm), all of its IPC receives
 * should go through this API rather than calling ipc_recv directly. */
#ifndef TOS_EVENT_H
#define TOS_EVENT_H

#include <lib/syscall.h>
#include <stdint.h>

#define EVENT_INBOX_CAPACITY 16
#define EVENT_WAIT_FOREVER UINT32_MAX

enum event_result {
    EVENT_ERROR   = -1,
    EVENT_FULL    = -2,
    EVENT_TIMEOUT = -3,
    EVENT_NONE    = 0,
    EVENT_READY   = 1,
};

typedef int (*event_match_fn)(const struct ipc_msg *message, void *context);

/* Receive the oldest message, preferring messages deferred by a filtered
 * wait before reading the kernel queue. */
int event_poll(struct ipc_msg *out);

/* Return the oldest message accepted by `match`. Messages rejected by the
 * predicate stay in their original order in a process-local inbox. */
int event_poll_matching(struct ipc_msg *out, event_match_fn match,
                        void *context);

/* As event_poll_matching, sleeping one tick between empty polls. A timeout
 * of EVENT_WAIT_FOREVER has no deadline. */
int event_wait_matching(struct ipc_msg *out, event_match_fn match,
                        void *context, uint32_t timeout_ticks);

/* Common type/sender filters. sender_pid <= 0 accepts any sender. */
int event_poll_type(struct ipc_msg *out, uint32_t type, int sender_pid);
int event_wait_type(struct ipc_msg *out, uint32_t type, int sender_pid,
                    uint32_t timeout_ticks);

int event_pending(void);

#endif

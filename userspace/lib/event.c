#include "event.h"

struct event_type_match {
    uint32_t type;
    int sender_pid;
};

static struct ipc_msg inbox[EVENT_INBOX_CAPACITY];
static int inbox_count;

static void inbox_remove(int index, struct ipc_msg *out) {
    *out = inbox[index];
    for (int i = index + 1; i < inbox_count; i++)
        inbox[i - 1] = inbox[i];
    inbox_count--;
}

static int inbox_append(const struct ipc_msg *message) {
    if (inbox_count >= EVENT_INBOX_CAPACITY)
        return EVENT_FULL;
    inbox[inbox_count++] = *message;
    return EVENT_READY;
}

static int type_matches(const struct ipc_msg *message, void *context) {
    const struct event_type_match *want = context;
    return message->type == want->type &&
           (want->sender_pid <= 0 ||
            (int)message->from_pid == want->sender_pid);
}

int event_poll(struct ipc_msg *out) {
    if (!out)
        return EVENT_ERROR;
    if (inbox_count > 0) {
        inbox_remove(0, out);
        return EVENT_READY;
    }
    return ipc_recv(out) > 0 ? EVENT_READY : EVENT_NONE;
}

int event_poll_matching(struct ipc_msg *out, event_match_fn match,
                        void *context) {
    if (!out || !match)
        return EVENT_ERROR;

    for (int i = 0; i < inbox_count; i++) {
        if (match(&inbox[i], context)) {
            inbox_remove(i, out);
            return EVENT_READY;
        }
    }

    /* The kernel ring has the same capacity. Drain enough messages to find
     * a match behind unrelated traffic without allowing a broken producer
     * to hold this non-blocking call forever. */
    for (int i = 0; i < EVENT_INBOX_CAPACITY; i++) {
        struct ipc_msg message;
        /* Do not consume a kernel message unless we can preserve it when it
         * is unrelated. The caller can drain one pending event and retry. */
        if (inbox_count >= EVENT_INBOX_CAPACITY)
            return EVENT_FULL;
        if (ipc_recv(&message) <= 0)
            return EVENT_NONE;
        if (match(&message, context)) {
            *out = message;
            return EVENT_READY;
        }
        int stored = inbox_append(&message);
        if (stored < 0)
            return stored;
    }
    return EVENT_NONE;
}

int event_wait_matching(struct ipc_msg *out, event_match_fn match,
                        void *context, uint32_t timeout_ticks) {
    if (!out || !match)
        return EVENT_ERROR;

    uint32_t started = (uint32_t)get_ticks();
    for (;;) {
        int result = event_poll_matching(out, match, context);
        if (result != EVENT_NONE)
            return result;
        if (timeout_ticks != EVENT_WAIT_FOREVER &&
            (uint32_t)((uint32_t)get_ticks() - started) >= timeout_ticks)
            return EVENT_TIMEOUT;
        sleep_ticks(1);
    }
}

int event_poll_type(struct ipc_msg *out, uint32_t type, int sender_pid) {
    struct event_type_match want = {type, sender_pid};
    return event_poll_matching(out, type_matches, &want);
}

int event_wait_type(struct ipc_msg *out, uint32_t type, int sender_pid,
                    uint32_t timeout_ticks) {
    struct event_type_match want = {type, sender_pid};
    return event_wait_matching(out, type_matches, &want, timeout_ticks);
}

int event_pending(void) {
    return inbox_count;
}

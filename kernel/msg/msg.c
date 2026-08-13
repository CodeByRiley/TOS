/* kernel/msg/msg.c — per-task input + IPC ring backends.
 *
 * Rings themselves live on the task struct (allocated in sched.c). This
 * module is the API that pushes/pops on them. Single-producer-single-
 * consumer per ring (IRQs post, syscalls pop), so we get away without
 * locks on UP. SMP path would need atomic head/tail.
 *
 * Input ownership: msg_post and msg_post_to route input events. A single
 * "input owner" pid receives all input via msg_post; per-task delivery
 * via msg_post_to bypasses the owner registry (used by the WM forwarding
 * events to focused client windows).
 */
#include <msg/msg.h>
#include <sched/sched.h>
#include <utilities/string.h>
#include <stdint.h>

#define INPUT_RING_SIZE 64
#define INPUT_RING_MASK (INPUT_RING_SIZE - 1)
#define IPC_RING_SIZE   16
#define IPC_RING_MASK   (IPC_RING_SIZE - 1)

#define _STATIC_ASSERT_POW2(n) _Static_assert(((n) & ((n) - 1)) == 0, #n " must be pow2")
_STATIC_ASSERT_POW2(INPUT_RING_SIZE);
_STATIC_ASSERT_POW2(IPC_RING_SIZE);

int msg_input_ring_size(void) { return INPUT_RING_SIZE; }
int msg_ipc_ring_size(void)   { return IPC_RING_SIZE; }

static volatile int input_owner_pid = 0;

void msg_init(void) {
    input_owner_pid = 0;
}

void msg_post_to(int pid, const struct msg *m) {
    if (pid <= 0) return;
    struct task *t = task_find(pid);
    if (!t || !t->input_ring) return;
    int next = (t->input_head + 1) & INPUT_RING_MASK;
    if (next == t->input_tail) return;          /* drop on overflow */
    t->input_ring[t->input_head] = *m;
    t->input_head = next;
}

void msg_post(const struct msg *m) {
    int owner = input_owner_pid;
    if (owner > 0) {
        msg_post_to(owner, m);
        return;
    }
    /* Fallback: deliver to init (pid=1) so the shell-only boot path still
     * sees keyboard events when no userspace winman is up. */
    msg_post_to(1, m);
}

int msg_get(struct msg *out) {
    struct task *t = task_current();
    if (!t || !t->input_ring) return 0;
    if (t->input_tail == t->input_head) return 0;
    *out = t->input_ring[t->input_tail];
    t->input_tail = (t->input_tail + 1) & INPUT_RING_MASK;
    return 1;
}

int msg_peek(struct msg *out) {
    struct task *t = task_current();
    if (!t || !t->input_ring) return 0;
    if (t->input_tail == t->input_head) return 0;
    *out = t->input_ring[t->input_tail];
    return 1;
}

int ipc_send(int target_pid, const struct ipc_msg *m, int from_pid) {
    if (target_pid <= 0 || !m) return -1;
    struct task *t = task_find(target_pid);
    if (!t || !t->ipc_ring) return -1;
    int next = (t->ipc_head + 1) & IPC_RING_MASK;
    if (next == t->ipc_tail) return -1;         /* full — caller can retry */
    struct ipc_msg copy = *m;
    copy.from_pid = (uint32_t)from_pid;
    t->ipc_ring[t->ipc_head] = copy;
    t->ipc_head = next;
    return 0;
}

int ipc_recv(struct ipc_msg *out) {
    struct task *t = task_current();
    if (!t || !t->ipc_ring) return 0;
    if (t->ipc_tail == t->ipc_head) return 0;
    *out = t->ipc_ring[t->ipc_tail];
    t->ipc_tail = (t->ipc_tail + 1) & IPC_RING_MASK;
    return 1;
}

int msg_input_owner_register(int pid) {
    if (input_owner_pid != 0 && input_owner_pid != pid) return -1;
    input_owner_pid = pid;
    return 0;
}

void msg_input_owner_force(int pid) {
    input_owner_pid = pid > 0 ? pid : 0;
}

void msg_input_owner_clear(int pid) {
    if (input_owner_pid == pid) input_owner_pid = 0;
}

int msg_input_owner(void) { return input_owner_pid; }

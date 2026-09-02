/* Host scheduler/IRQ adapter. The real VFS gate and FIFO run unchanged. */
#include <sched/sched.h>
#include <pthread.h>
#include "vfs_lock_host.h"
#include <time.h>

static pthread_mutex_t irq_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static unsigned parked;
static _Thread_local struct task current;
static _Thread_local unsigned irq_disabled;
static _Thread_local int cpu_id, in_irq, bootstrap;

struct task *task_current(void) { return bootstrap ? 0 : &current; }
int percpu_current_id(void) { return cpu_id; }
int irq_in_handler(void) { return in_irq; }
void vfs_test_context(int cpu, int irq, int boot) {
    cpu_id = cpu; in_irq = irq; bootstrap = boot;
}

u64 vfs_test_irq_save(void) {
    assert(!irq_disabled);
    assert(!pthread_mutex_lock(&irq_mutex));
    irq_disabled = 1;
    return 1ull << 9;
}
void vfs_test_irq_restore(u64 flags) {
    assert(irq_disabled && (flags & (1ull << 9)));
    irq_disabled = 0;
    assert(!pthread_mutex_unlock(&irq_mutex));
}
static void wait_changed(void) {
    struct timespec deadline;
    timespec_get(&deadline, TIME_UTC);
    deadline.tv_sec += 10;
    assert(!pthread_cond_timedwait(&changed, &irq_mutex, &deadline));
}
void task_block(int waiting_for_pid) {
    assert(irq_disabled && current.vfs_active && waiting_for_pid == -1);
    current.state = TASK_BLOCKED;
    parked++;
    pthread_cond_broadcast(&changed);
    while (current.state == TASK_BLOCKED)
        wait_changed();
    parked--;
    current.state = TASK_RUNNING;
}
void task_wakeup(struct task *task) {
    assert(irq_disabled && task->vfs_active && task->state == TASK_BLOCKED);
    task->state = TASK_READY;
    pthread_cond_broadcast(&changed);
}

/* Deterministic queue tests: no timing guesses or arbitrary sleeps. */
void vfs_test_wait_parked(unsigned count) {
    assert(!pthread_mutex_lock(&irq_mutex));
    while (parked < count)
        wait_changed();
    assert(!pthread_mutex_unlock(&irq_mutex));
}

__attribute__((noreturn))
void panic_at(const char *message, const char *file, int line, const char *func) {
    fprintf(stderr, "VFS panic: %s (%s:%d %s)\n", message, file, line, func);
    _Exit(86);
}

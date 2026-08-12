#include "../include/sys/pthread.h"
#include "../include/stdlib.h"
#include <stdint.h>

// Your underlying syscall wrappers
extern long thread_create(void *(*entry)(void *), void *stack, void *arg);
extern long thread_exit();
extern long futex_wait(uint32_t *addr, uint32_t expected);
extern long futex_wake(uint32_t *addr);

int pthread_create(pthread_t *thread,
                   const pthread_attr_t *attr,
                   void *(*start_routine)(void *),
                   void *arg) {
    (void)attr;

    // Allocate the user stack (16 KB is safe)
    void *stack = malloc(16384);
    if (!stack) return -1;

    // System V ABI: stack must be 16-byte aligned before call, so RSP % 16 == 8 on entry
    void *stack_top = (char*)stack + 16384 - 8;

    // Cast start_routine to void(*)(void*) so it matches the syscall prototype.
    // The kernel will put `arg` into RDI, so the start_routine receives it perfectly!
    long tid = thread_create(start_routine, stack_top, arg);
    if (tid < 0) {
        free(stack);
        return -1;
    }

    *thread = (pthread_t)tid;
    return 0;
}

void pthread_exit(void *retval) {
    (void)retval;
    thread_exit();
}

int pthread_join(pthread_t thread, void **retval) {
    (void)retval; // We don't support return values yet

    long rc = thread_join((long)thread);
    return (int)rc;
}

// --- Mutexes (Futex-backed) ---

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr) {
    (void)attr;
    __atomic_store_n(mutex, 0, __ATOMIC_RELEASE);
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
    // Spin in userspace trying to acquire the lock (0 -> 1)
    while (__atomic_exchange_n(mutex, 1, __ATOMIC_ACQUIRE) != 0) {
        // If it was already 1, ask the kernel to sleep us until it becomes 0
        futex_wait((uint32_t*)mutex, 1);
        // When we wake up, loop around and try the atomic exchange again
    }
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    // Release the lock (1 -> 0)
    __atomic_store_n(mutex, 0, __ATOMIC_RELEASE);

    // Wake up ONE thread that is waiting on this mutex
    futex_wake((uint32_t*)mutex);
    return 0;
}

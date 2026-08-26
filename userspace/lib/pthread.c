#include <include/sys/pthread.h>
#ifdef TOS_USE_MUSL
#include <stdlib.h>
#else
#include <include/stdlib.h>
#endif
#include <stdint.h>

extern long thread_create(void *(*entry)(void *), void *stack, void *arg);
extern long thread_exit();
extern long futex_wait(uint32_t *addr, uint32_t expected);
extern long futex_wake(uint32_t *addr);

static struct thread_node *thread_list = NULL;

static pthread_mutex_t list_mutex = 0;

void pthread_entry(struct pthread_arg *p) {
    p->start_routine(p->arg);
    free(p);
    thread_exit();
}

int pthread_create(pthread_t *thread,
                   const pthread_attr_t *attr,
                   void *(*start_routine)(void *),
                   void *arg) {
    (void)attr;

    void *stack = malloc(16384);
    if (!stack) return -1;

    struct pthread_arg *parg = malloc(sizeof(struct pthread_arg));
    if (!parg) { free(stack); return -1; }

    parg->start_routine = start_routine;
    parg->arg = arg;

    /* SysV requires 16-byte stack alignment before a call. */
    void *stack_top = (char*)stack + 16384 - 8;

    long tid = thread_create((void*)pthread_entry, stack_top, parg);
    if (tid < 0) {
        free(stack);
        free(parg);
        return -1;
    }

    /* Track the stack for join cleanup. */
    struct thread_node *node = malloc(sizeof(struct thread_node));
    if (!node) {
        /* TODO: Terminate the new thread if its join record cannot be stored. */
        free(stack);
        free(parg);
        return -1;
    }
    node->tid = tid;
    node->stack_base = stack;

    pthread_mutex_lock(&list_mutex);
    node->next = thread_list;
    thread_list = node;
    pthread_mutex_unlock(&list_mutex);

    *thread = (pthread_t)tid;
    return 0;
}

int pthread_join(pthread_t thread, void **retval) {
    (void)retval; /* Return values are unsupported. */

    long tid = (long)thread;

    long rc = thread_join(tid);

    if (rc == 0) {
        pthread_mutex_lock(&list_mutex);

        struct thread_node **curr = &thread_list;
        while (*curr) {
            if ((*curr)->tid == tid) {
                struct thread_node *target = *curr;
                *curr = target->next;
                free(target->stack_base);
                free(target);
                break;
            }
            curr = &(*curr)->next;
        }

        pthread_mutex_unlock(&list_mutex);
    }

    return (int)rc;
}

void pthread_exit(void *retval) {
    (void)retval;
    thread_exit();
}

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr) {
    (void)attr;
    __atomic_store_n(mutex, 0, __ATOMIC_RELEASE);
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
    while (__atomic_exchange_n(mutex, 1, __ATOMIC_ACQUIRE) != 0) {
        futex_wait((uint32_t*)mutex, 1);
    }
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    __atomic_store_n(mutex, 0, __ATOMIC_RELEASE);
    futex_wake((uint32_t*)mutex);
    return 0;
}

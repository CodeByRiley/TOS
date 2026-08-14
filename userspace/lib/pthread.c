#include <include/sys/pthread.h>
#include <include/stdlib.h>
#include <stdint.h>

extern long thread_create(void *(*entry)(void *), void *stack, void *arg);
extern long thread_exit();
extern long futex_wait(uint32_t *addr, uint32_t expected);
extern long futex_wake(uint32_t *addr);

static struct thread_node *thread_list = NULL;

static pthread_mutex_t list_mutex = 0;

void pthread_entry(struct pthread_arg *p) {
    // Run the actual thread function
    p->start_routine(p->arg);

    // Free the argument struct (allocated in pthread_create)
    free(p);

    // Tell the kernel to kill the thread
    thread_exit();
}

// --- Thread Creation ---
int pthread_create(pthread_t *thread,
                   const pthread_attr_t *attr,
                   void *(*start_routine)(void *),
                   void *arg) {
    (void)attr;

    // 1. Allocate the stack
    void *stack = malloc(16384);
    if (!stack) return -1;

    // 2. Allocate the argument payload
    struct pthread_arg *parg = malloc(sizeof(struct pthread_arg));
    if (!parg) { free(stack); return -1; }

    parg->start_routine = start_routine;
    parg->arg = arg;

    // System V ABI: stack must be 16-byte aligned before call
    void *stack_top = (char*)stack + 16384 - 8;

    // 3. Tell the kernel to start the thread
    long tid = thread_create((void*)pthread_entry, stack_top, parg);
    if (tid < 0) {
        free(stack);
        free(parg);
        return -1;
    }

    // 4. Add to our tracking list so join can free it later
    struct thread_node *node = malloc(sizeof(struct thread_node));
    if (!node) {
        // If we can't track it, we can't join it. Abort.
        // (In a real OS, you'd kill the thread here, but we'll just return an error)
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

// --- Thread Joining ---
int pthread_join(pthread_t thread, void **retval) {
    (void)retval; // Return values not supported yet

    long tid = (long)thread;

    // 1. Wait for the kernel to finish the thread
    long rc = thread_join(tid);

    if (rc == 0) {
        // 2. Thread is dead. Find its stack base in our list and free it.
        pthread_mutex_lock(&list_mutex);

        struct thread_node **curr = &thread_list;
        while (*curr) {
            if ((*curr)->tid == tid) {
                struct thread_node *target = *curr;
                *curr = target->next; // Unlink from list

                free(target->stack_base); // FREE THE 16KB STACK!
                free(target);             // FREE THE LIST NODE!
                break;
            }
            curr = &(*curr)->next;
        }

        pthread_mutex_unlock(&list_mutex);
    }

    return (int)rc;
}

// --- Mutexes ---
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

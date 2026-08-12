#include "../../include/stdio.h"
#include "../../include/sys/pthread.h"

volatile int t1_counter = 0;
volatile int t2_counter = 0;
volatile uint32_t t1_mutex = 0;
volatile uint32_t t2_mutex = 0;

void *thread_func(void *arg) {
    (void)arg;
    printf("Hello from thread!\n");

    for (int i = 0; i < 1000000; i++) {
        // Lock
        while (__atomic_exchange_n(&t1_mutex, 1, __ATOMIC_ACQUIRE) != 0) {
            futex_wait((uint32_t*)&t1_mutex, 1);
        }

        t1_counter++;

        // Unlock
        __atomic_store_n(&t1_mutex, 0, __ATOMIC_RELEASE);
        futex_wake((uint32_t*)&t1_mutex);
    }

    printf("Thread finished! Counter is %d\n", t1_counter);
    thread_exit();
    return NULL;
}

void *thread_func1(void *arg) {
    (void)arg;
    printf("Hello from thread!\n");

    for (int i = 0; i < 1000000; i++) {
        // Lock
        while (__atomic_exchange_n(&t2_mutex, 1, __ATOMIC_ACQUIRE) != 0) {
            futex_wait((uint32_t*)&t2_mutex, 1);
        }

        t2_counter++;

        // Unlock
        __atomic_store_n(&t2_mutex, 0, __ATOMIC_RELEASE);
        futex_wake((uint32_t*)&t2_mutex);
    }

    printf("Thread finished! Counter is %d\n", t2_counter);
    thread_exit();
    return NULL;
}

int main() {
    printf("Hello from main!\n");

    void *t1_stack = malloc(16384);
    void *t2_stack = malloc(16384);
    if (!t1_stack || !t2_stack) return 1;

    // void *t1_stack_top = (char*)t1_stack + 16384 - 8;
    // void *t2_stack_top = (char*)t2_stack + 16384 - 8;

    pthread_t tid1, tid2;
    pthread_create(&tid1, NULL, thread_func, NULL);
    pthread_create(&tid2, NULL, thread_func1, NULL);

    printf("Main created threads %lu %lu\n", tid1, tid2);

    // Safely wait for both threads to completely finish!
    pthread_join(tid1, NULL);
    pthread_join(tid2, NULL);

    printf("Main exiting. Final counter: %d %d\n", t1_counter, t2_counter);

    // Now it is 100% safe to free the stacks!
    free(t1_stack);
    free(t2_stack);
    return 1;
}

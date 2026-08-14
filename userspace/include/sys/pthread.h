#ifndef _SYS_PTHREAD_H
#define _SYS_PTHREAD_H

#include <include/stdio.h>
#include <include/stdlib.h>
#include <stdint.h>

typedef unsigned long pthread_t;
typedef volatile uint32_t pthread_mutex_t;
typedef int pthread_attr_t;
typedef int pthread_mutexattr_t;

// We need to keep track of the stack bases so pthread_join can free them.
struct thread_node {
    long tid;
    void *stack_base;
    struct thread_node *next;
};

struct pthread_arg {
    void *(*start_routine)(void*);
    void *arg;
};


void pthread_entry(struct pthread_arg *p);
int pthread_create(pthread_t *thread,
                   const pthread_attr_t *attr,
                   void *(*start_routine)(void *),
                   void *arg);
void pthread_exit(void *retval);
int pthread_join(pthread_t thread, void **retval);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);

#endif /* _SYS_PTHREAD_H */

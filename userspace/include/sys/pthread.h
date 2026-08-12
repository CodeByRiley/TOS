/* userspace/include/sys/pthread.h — pthread placeholder.
 *
 * Empty on purpose. There are no userspace threads yet; ported code that
 * #includes <sys/pthread.h> compiles but gets no symbols. Fill this in
 * when a real threading primitive lands.
 */
#ifndef _SYS_PTHREAD_H
#define _SYS_PTHREAD_H

#include "../stdio.h"
#include "../stdlib.h"
#include <stdint.h>

typedef unsigned long pthread_t;
typedef volatile uint32_t pthread_mutex_t;
typedef int pthread_attr_t;
typedef int pthread_mutexattr_t;

struct pthread {
	void *(*start_routine)(void *);
	void *arg;
};

struct pthread_arg {
    void *(*start_routine)(void*);
    void *arg;
};

typedef volatile uint32_t pthread_mutex_t;

void pthread_entry(struct pthread *p);
int pthread_create(pthread_t *thread,
                   const pthread_attr_t *attr,
                   void *(*start_routine)(void *),
                   void *arg);
void pthread_exit(void *retval);
int pthread_join(pthread_t thread, void **retval);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);

#endif /* _SYS_PTHREAD_H */
// how the fuck did I forget this for so lonh

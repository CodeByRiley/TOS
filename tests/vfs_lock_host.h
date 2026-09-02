/* Shared host checks: kernel/assert.h intentionally spins and must not be
 * used by these tests. A failed check must terminate with a useful message. */
#ifndef VFS_LOCK_HOST_H
#define VFS_LOCK_HOST_H
#include <stdio.h>
#include <stdlib.h>
#undef assert
#define assert(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        abort(); \
    } \
} while (0)

void vfs_test_wait_parked(unsigned count);
void vfs_test_context(int cpu, int irq, int boot);
#endif

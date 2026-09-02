/* One serialization boundary; backend callbacks run with it already held. */
#ifndef VFS_LOCK_H
#define VFS_LOCK_H

void vfs_lock(void);
void vfs_unlock(void);
void vfs_assert_locked(void);

/* Scope cleanup covers every early return without a forest of unlock labels.
 * Do not nest guards: call already-locked helpers inside a VFS operation. */
static inline void vfs_guard_release(int *guard) {
    (void)guard;
    vfs_unlock();
}
#define VFS_GUARD() \
    int vfs_guard __attribute__((cleanup(vfs_guard_release), unused)) = (vfs_lock(), 0)

#endif

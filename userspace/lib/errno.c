/* userspace/lib/errno.c — definition of the global `errno`.
 *
 * Single translation unit owns the storage; headers expose it via extern.
 * No thread-local variant: this OS is single-threaded per process for now.
 */
int errno = 0;

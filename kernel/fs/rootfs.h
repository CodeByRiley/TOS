/* kernel/fs/rootfs.h , boot-time root filesystem selection.
 *
 * Implementation: kernel/fs/rootfs.c.
 */
#ifndef KERNEL_ROOTFS_H
#define KERNEL_ROOTFS_H

#include <utilities/types.h>

/* Register the filesystem types, then mount a root at "/" from the first
 * volume any of them recognises, and attach USB volumes underneath it.
 * Returns 0 once "/" is mounted, -1 when nothing anywhere could be mounted ,
 * which the caller should treat as fatal, since nothing else can start. */
int rootfs_mount(u64 mb2_addr);

#endif

/* kernel/drivers/storage/blockdev.h , named registry of mountable volumes.
 *
 * struct block_device says how to read a volume but not which volume it is.
 * Mounting from userspace needs a name, so every transport publishes what it
 * finds here under transport-plus-unit ("ahci0", "usb1") and the mount syscall
 * resolves that name back to a transport. A partition published by
 * drivers/storage/partition.h appends "p" and its number ("ahci0p1").
 *
 * Registration BORROWS the adapter behind device->context: the adapter must
 * outlive the registration, which in practice means it is static. Lookups copy
 * the descriptor out rather than lending a pointer into the table, because a
 * mount blocks for the whole time it reads a volume in.
 *
 * There is no unregister. Nothing hot-unplugs a transport yet, and a mount
 * holding a copy of the descriptor would not notice if it did.
 *
 * Implementation: kernel/drivers/storage/blockdev.c.
 */
#ifndef STORAGE_BLOCKDEV_H
#define STORAGE_BLOCKDEV_H

#include <arch/syscall_abi.h>
#include <drivers/storage/block.h>
#include <stddef.h>
#include <stdint.h>

/* 32 AHCI ports and 8 USB slots, plus room for the partitions they carry. */
#define BLOCKDEV_MAX 64

/* `start_lba` is where this volume begins inside its parent disk, and is 0 for
 * a whole disk. `flags` carries the BLOCKDEV_* bits the caller knows about;
 * BLOCKDEV_WRITABLE is derived from the device itself and need not be passed.
 * Returns 0, or -1 on a duplicate or over-long name, a device with no read
 * callback or no sectors, or a full table. */
int blockdev_register(const char *name, const struct block_device *device,
                      uint64_t start_lba, uint32_t flags);

/* OR extra bits into an already-published volume. A disk is only known to be
 * partitioned once its table has been read, which happens after it is named. */
int blockdev_set_flags(const char *name, uint32_t flags);

size_t blockdev_count(void);

/* Describe entry `index` for enumeration. 0 on success, -1 past the end.
 * Entries keep their index for the life of the machine, so a walk that stops
 * at the first -1 sees every volume. */
int blockdev_describe(size_t index, struct blockdev_info *out);

/* Resolve a published name to a usable transport. 0 on success, -1 if no
 * volume goes by that name. */
int blockdev_lookup(const char *name, struct block_device *out);

#endif

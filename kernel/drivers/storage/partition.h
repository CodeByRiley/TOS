/* kernel/drivers/storage/partition.h , MBR partition tables.
 *
 * A disk that carries a partition table has no filesystem at LBA 0 -- it has
 * a 512-byte table there and its filesystems live at offsets recorded in it.
 * Handing such a disk straight to a filesystem is how a perfectly good volume
 * looks unformatted, so every published disk is scanned here first and each
 * partition is published as a volume of its own.
 *
 * A partition is an ordinary struct block_device that adds its start LBA to
 * every request, so nothing above this layer knows the difference. The slice
 * records are static for the same reason the transports are: a mount borrows
 * one for as long as it lives.
 *
 * GPT is not supported. A protective MBR is recognised and skipped rather
 * than mistaken for a real partition covering the disk.
 *
 * Implementation: kernel/drivers/storage/partition.c.
 */
#ifndef STORAGE_PARTITION_H
#define STORAGE_PARTITION_H

#include <drivers/storage/block.h>
#include <stdint.h>

/* Slices across every disk in the machine. */
#define PARTITION_MAX 32

/* Read `parent`'s partition table and publish each usable partition under
 * `parent_name` plus "p" and its number, carrying `inherit` (the parent's
 * BLOCKDEV_* bits, so a partition of a removable disk stays removable).
 *
 * Returns how many partitions were published. Zero means the disk carries no
 * partition table the caller should defer to -- an unreadable sector, an
 * absent signature, a GPT, or a table whose every entry is empty or bogus --
 * and the caller should go on treating the whole device as one volume. */
int partition_scan(const struct block_device *parent, const char *parent_name,
                   uint32_t inherit);

#endif

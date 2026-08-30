/* kernel/fs/fat/ahci/fat_ahci.h , AHCI storage backend for the FAT driver.
 *
 * fat.c operates on a RAM image and knows nothing about disks. This is the
 * half that does: it loads the image off an AHCI port at mount time and
 * installs the write-through hook that pushes dirtied sectors back.
 *
 * Keeping it in its own translation unit is what keeps fat.c free of AHCI
 * and kernel-heap symbols, so the host tests can link the filesystem logic
 * on its own. Anything that needs a disk behind the image includes this;
 * everything else includes fs/fat/fat.h and stays portable.
 *
 * Implementation: kernel/fs/fat/ahci/fat_ahci.c.
 */
#ifndef FAT_AHCI_H
#define FAT_AHCI_H

#include <stdint.h>
#include <drivers/storage/ahci.h>

/* Read the whole volume on `port` into RAM, bind the FAT driver to it, and
 * install the write-through backend. Returns 0 on success, -1 otherwise. */
int  fat_mount_from_ahci(struct AHCI_DEVICE_DATA *ahci_dev, int port);

/* Write the entire mounted image back to the disk it came from. */
void fat_flush(void);

/* Read one 512-byte sector straight off the disk, bypassing the image. */
int  fat_read_sector(u32 lba, void *buf);

#endif

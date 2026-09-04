/* kernel/fs/rootfs.c , finding and mounting the root filesystem at boot.
 *
 * Every filesystem now mounts the same way , give it a struct block_device and
 * a mountpoint , so choosing a root is a search rather than a ladder: walk the
 * transports the drivers discovered, and for each one offer the volume to each
 * registered filesystem until one recognises it.
 *
 * Order is deliberate. Real disks come before the Multiboot ramdisk so that a
 * machine with a formatted disk boots from it rather than from the image GRUB
 * happened to load. Within a disk, ext2 is tried before FAT because it is the
 * one that validates a superblock; FAT accepts anything with a plausible BPB.
 *
 * A filesystem that fails to mount must leave nothing behind, so a failed
 * attempt is followed by the next candidate on the same open device, and a
 * device that nothing claims is closed before moving on.
 *
 * Implementation of the mount contracts themselves lives with each backend;
 * see kernel/fs/README.md.
 */
#include <boot/multiboot2.h>
#include <drivers/storage/ahci.h>
#include <drivers/storage/ahci_block.h>
#include <drivers/storage/block.h>
#include <drivers/usb/storage/usb_storage.h>
#include <fs/ext2/ext2.h>
#include <fs/fat/fat_vfs.h>
#include <fs/rootfs.h>
#include <fs/vfs/vfs.h>
#include <memory/hhdm.h>
#include <utilities/log.h>

/* Filesystems to offer a newly opened volume, most discriminating first: ext2
 * validates a superblock, while FAT accepts anything with a plausible BPB. */
static const struct {
    const char *name;
    int (*mount)(const char *mountpoint, const struct block_device *device);
} filesystems[] = {
    {"ext2", ext2_mount_device},
    {"fat", fat_mount_device},
};

static int mount_any_fs(const char *path, const struct block_device *device,
                        const char *source) {
    for (usize i = 0; i < sizeof(filesystems) / sizeof(filesystems[0]); i++) {
        if (filesystems[i].mount(path, device) != 0)
            continue;
        log_write_fmt(FILESYS, LOG_INFO, "rootfs: %s mounted from %s at %s",
                      filesystems[i].name, source, path);
        return 0;
    }
    return -1;
}

/* The adapter must outlive every mount using it, so the root disk is static
 * rather than a stack record handed to the filesystem and then dropped. */
static struct ahci_block_device root_disk;

static int mount_from_ahci(const char *path) {
    if (!g_ahci_dev)
        return -1;
    for (int port = 0; port < AHCI_MAX_PORTS; port++) {
        if (ahci_block_open(&root_disk, g_ahci_dev, port) != 0)
            continue;
        if (mount_any_fs(path, &root_disk.device, "AHCI") == 0)
            return 0;
        ahci_block_close(&root_disk);
    }
    return -1;
}

static int mount_from_ramdisk(const char *path, u64 mb2_addr) {
    struct MB2_TAG_MODULE *module = mb2_find_module(mb2_addr, "rootfs");
    if (!module)
        return -1;
    const char *fstype = 0;
    if (vfs_mount_auto(path, phys_to_virt(module->mod_start),
                       module->mod_end - module->mod_start, &fstype) != 0)
        return -1;
    log_write_string("rootfs: mounted from Multiboot2 ramdisk module",
                     fstype ? fstype : "unknown", FILESYS, LOG_INFO);
    return 0;
}

/* USB volumes are extra mounts, not root candidates: they are attached only
 * once a root exists, because their synthetic mountpoints need one. */
static void mount_usb_volumes(void) {
    for (usize i = 0; i < usb_storage_count(); i++) {
        char path[] = "/usb0";
        path[4] = (char)('0' + i);
        if (mount_any_fs(path, usb_storage_device(i), "USB") != 0)
            log_write_string("rootfs: no supported volume on", path, FILESYS,
                             LOG_WARN);
    }
}

int rootfs_mount(u64 mb2_addr) {
    vfs_init();
    ext2_vfs_register();
    fat_vfs_register();

    if (mount_from_ahci("/") != 0) {
        log_write("rootfs: no AHCI volume, trying ramdisk", FILESYS, LOG_WARN);
        if (mount_from_ramdisk("/", mb2_addr) != 0)
            return -1;
    }

    mount_usb_volumes();
    return 0;
}

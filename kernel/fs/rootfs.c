/* kernel/fs/rootfs.c , publishing storage volumes and mounting a root.
 *
 * Boot storage happens in two steps. First every transport the drivers found
 * is opened, scanned for a partition table, and published under a name
 * (kernel/drivers/storage/blockdev.h), so that a volume is addressable for the
 * rest of the machine's life , that is what SYS_MOUNT resolves its `source`
 * against. Then a root is chosen by walking those published volumes and
 * offering each to the VFS until some filesystem claims it.
 *
 * Order is deliberate. Real disks come before the Multiboot ramdisk so that a
 * machine with a formatted disk boots from it rather than from the image GRUB
 * happened to load. Within the registry, volumes come out in publication
 * order, which is port order with each disk's partitions right behind it.
 * Which filesystem wins a given volume is decided by registration order in
 * rootfs_mount, since vfs_attach_auto tries them in that order: ext2 first
 * because it validates a superblock, FAT second because it accepts anything
 * with a plausible BPB.
 *
 * Two kinds of volume are never root candidates. A PARTITIONED disk holds a
 * table rather than a filesystem, so its slices are tried instead of it. A
 * REMOVABLE one is mounted underneath the root instead, because its synthetic
 * mountpoint needs a root to exist first.
 *
 * A filesystem that fails to mount must leave nothing behind, so a failed
 * attempt is simply followed by the next candidate. Adapters are NOT closed
 * when nothing claims their volume: an unmounted disk is still a mountable
 * one, and the adapter has to stay alive for a later mount to borrow. The
 * standing cost is one bounce frame per live SATA port.
 *
 * Implementation of the mount contracts themselves lives with each backend;
 * see kernel/fs/README.md.
 */
#include <boot/multiboot2.h>
#include <drivers/storage/ahci.h>
#include <drivers/storage/ahci_block.h>
#include <drivers/storage/block.h>
#include <drivers/storage/blockdev.h>
#include <drivers/storage/partition.h>
#include <drivers/usb/storage/usb_storage.h>
#include <fs/ext2/ext2.h>
#include <fs/fat/fat_vfs.h>
#include <fs/rootfs.h>
#include <fs/vfs/vfs.h>
#include <memory/hhdm.h>
#include <utilities/log.h>

static int mount_any_fs(const char *path, const struct block_device *device,
                        const char *source) {
    const char *fstype = 0;
    if (vfs_attach_auto(path, (void *)device, &fstype) != 0)
        return -1;
    log_write_fmt(FILESYS, LOG_INFO, "rootfs: %s mounted from %s at %s",
                  fstype, source, path);
    return 0;
}

/* An adapter must outlive every mount using it, so the pool is static rather
 * than a stack record handed to a filesystem and then dropped. Indexed by port
 * so a published name says which port it came from; a zero controller marks a
 * slot that never opened. */
static struct ahci_block_device ahci_disks[AHCI_MAX_PORTS];

static void name_with_unit(char *out, const char *prefix, unsigned unit) {
    usize at = 0;
    while (prefix[at]) {
        out[at] = prefix[at];
        at++;
    }
    if (unit >= 10)
        out[at++] = (char)('0' + unit / 10);
    out[at++] = (char)('0' + unit % 10);
    out[at] = 0;
}

/* Publish one whole disk, then whatever its partition table describes. The
 * disk is named first so it keeps the lower index, and marked PARTITIONED
 * afterwards because whether it holds a table is only known once it is read. */
static void publish_disk(const struct block_device *device, const char *name,
                         u32 flags) {
    if (blockdev_register(name, device, 0, flags) != 0) {
        log_write_string("rootfs: could not publish", name, FILESYS, LOG_WARN);
        return;
    }
    if (partition_scan(device, name, flags) > 0)
        blockdev_set_flags(name, BLOCKDEV_PARTITIONED);
}

/* Open every SATA port that answers and publish it. Ports are probed in order
 * and named by port number rather than by discovery rank, so a disk keeps the
 * same name when an earlier port is empty. */
static void publish_ahci(void) {
    if (!g_ahci_dev)
        return;
    for (int port = 0; port < AHCI_MAX_PORTS; port++) {
        if (ahci_block_open(&ahci_disks[port], g_ahci_dev, port) != 0)
            continue;
        char name[BLOCKDEV_NAME_MAX];
        name_with_unit(name, "ahci", (unsigned)port);
        publish_disk(&ahci_disks[port].device, name, 0);
    }
}

static void publish_usb(void) {
    for (usize i = 0; i < usb_storage_count(); i++) {
        char name[BLOCKDEV_NAME_MAX];
        name_with_unit(name, "usb", (unsigned)i);
        publish_disk(usb_storage_device(i), name, BLOCKDEV_REMOVABLE);
    }
}

/* Offer every fixed volume to the VFS in publication order. The registry is
 * append-only, so stopping at the first gap sees all of them. */
static int mount_root(const char *path) {
    struct blockdev_info info;
    for (usize i = 0; blockdev_describe(i, &info) == 0; i++) {
        if (info.flags & (BLOCKDEV_PARTITIONED | BLOCKDEV_REMOVABLE))
            continue;
        struct block_device device;
        if (blockdev_lookup(info.name, &device) != 0)
            continue;
        if (mount_any_fs(path, &device, info.name) == 0)
            return 0;
    }
    return -1;
}

static int mount_from_ramdisk(const char *path, u64 mb2_addr) {
    struct MB2_TAG_MODULE *module = mb2_find_module(mb2_addr, "rootfs");
    if (!module) {
        /* Distinct from "the module would not mount": GRUB never handed one
         * over, which is a boot configuration problem and not a disk one. */
        log_write("rootfs: no Multiboot2 module named rootfs", FILESYS,
                  LOG_ERROR);
        return -1;
    }
    const char *fstype = 0;
    if (vfs_mount_auto(path, phys_to_virt(module->mod_start),
                       module->mod_end - module->mod_start, &fstype) != 0) {
        log_write("rootfs: ramdisk module holds no filesystem we recognise",
                  FILESYS, LOG_ERROR);
        return -1;
    }
    log_write_string("rootfs: mounted from Multiboot2 ramdisk module",
                     fstype ? fstype : "unknown", FILESYS, LOG_INFO);
    return 0;
}

/* Removable volumes are extra mounts, not root candidates: they are attached
 * only once a root exists, because their synthetic mountpoints need one. Each
 * mountable volume takes the next /usbN, so a stick with two partitions lands
 * on /usb0 and /usb1 rather than fighting over one name. */
static void mount_removable_volumes(void) {
    struct blockdev_info info;
    unsigned unit = 0;
    for (usize i = 0; blockdev_describe(i, &info) == 0; i++) {
        if (!(info.flags & BLOCKDEV_REMOVABLE) ||
            (info.flags & BLOCKDEV_PARTITIONED))
            continue;
        struct block_device device;
        if (blockdev_lookup(info.name, &device) != 0)
            continue;
        char path[] = "/usb0";
        path[4] = (char)('0' + unit);
        if (mount_any_fs(path, &device, info.name) != 0) {
            log_write_string("rootfs: no supported volume on", info.name,
                             FILESYS, LOG_WARN);
            continue;
        }
        if (++unit > 9)
            break;
    }
}

int rootfs_mount(u64 mb2_addr) {
    vfs_init();
    ext2_vfs_register();
    fat_vfs_register();

    publish_ahci();
    publish_usb();

    if (mount_root("/") != 0) {
        log_write("rootfs: no disk volume, trying ramdisk", FILESYS, LOG_WARN);
        if (mount_from_ramdisk("/", mb2_addr) != 0)
            return -1;
    }

    mount_removable_volumes();
    return 0;
}

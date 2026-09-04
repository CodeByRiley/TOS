# Filesystem layout

The VFS owns names and open-file lifetime. FAT and ext2 own disk layout.
This follows the object separation in the [Linux VFS](https://www.kernel.org/doc/html/latest/filesystems/vfs.html),
without importing its caches, locking machinery or full POSIX feature set.

## Where code belongs

| File | Responsibility |
| --- | --- |
| `vfs/vfs.h` | Caller API, backend contracts and object definitions |
| `vfs/internal.h` | Shared declarations used only inside the VFS |
| `vfs/vfs.c` | Filesystem registration, mounts and live inode references |
| `vfs/namei.c` | Component lookup, parent traversal, stat and name mutations |
| `vfs/file.c` | Open/close, byte I/O and directory enumeration |
| `vfs/lock.c`, `vfs/lock.h` | Sleeping FIFO gate and shared scope guard |
| `stdio.c` | Kernel `FILE *` API over the VFS |
| `fat/fat.c` | Legacy FAT path API and shared component mutations |
| `fat/fat_mount.c` | Volume validation, geometry, cluster chains and persistence |
| `fat/fat_file.c` | File transfer, cursor movement and truncation |
| `fat/fat_directory.c` | Directory cursors, entries and slot allocation |
| `fat/fat_name.c` | Long-name encoding and DOS 8.3 aliases |
| `fat/fat_vfs.c` | Translate FAT entries to VFS objects |
| `fat/fat_internal.h` | Shared FAT layouts, constants and helper declarations |
| `fat/fat_block.c` | Reading a FAT volume off a block device and writing back |
| `ext2/ext2_mount.c` | Superblock, feature and geometry validation |
| `ext2/ext2_io.c` | Device-backed cache, dirty-block writeback and barriers |
| `ext2/ext2_inode.c` | Inodes, block allocation and indirect blocks |
| `ext2/ext2_dir.c` | Directory entries and component lookup |
| `ext2/ext2_file.c` | Sparse file data transfers |
| `ext2/ext2_vfs.c` | Translate ext2 inodes to VFS objects |

FAT16 and FAT32 use the same directory algorithms. Their differences are
geometry and cluster encoding, so there are no duplicated format operation
tables or one-function selector files. Shared logic uses ordinary functions;
macros cover constants, on-disk layouts and the VFS scope guard.

## Objects and ownership

- A **filesystem type** describes how to mount a format. Registration borrows
  its static descriptor.
- A **superblock** owns a mounted instance's backend state and root inode
  reference. RAM images remain caller-owned; device mounts own their cache.
- An **inode** identifies an object within that superblock. Live references
  to the same inode number share one object. Backend metadata is borrowed
  from the mounted image; it is not copied into each open handle.
- A **dentry** temporarily associates a path component with an inode and its
  parent. The lookup chain is released after each operation; no name cache
  needs invalidating on create/remove.
- A **file** owns an inode reference and an independent position. Closing it
  releases backend per-open state and the reference. Do not shallow-copy an
  open `vfs_file` or open over one without closing it first.

For `/mnt/docs/note`, the VFS walks each component, enters the mount at
`/mnt`, then calls the mounted backend's `lookup(directory, name)`.
Backends never receive the entire path. `..` follows the lookup chain, so
`/mnt/..` returns to the containing filesystem. Missing components and
non-directories are checked before interpreting subsequent `..` components.

`lookup` and `create` return one owned inode reference on success and none
on failure. Use `vfs_inode_get` to share identity, and `vfs_inode_put` to drop
ownership. `open` may allocate per-open state; `release` must accept partial
initialization. Failed mount setup calls `unmount`, which must likewise
accept partial state. Unmount releases the root before backend state.

## Serialization

Every public VFS operation takes one non-recursive sleeping gate. It covers
mount publication, lookup, inode references, file offsets, backend mutations,
sync, close and unmount. Backend callbacks and inode-reference helpers run
with the gate already held; they must not re-enter the public API. Private
`*_locked` helpers handle nested work. `VFS_GUARD()` releases on every return,
including allocation and I/O errors.

This gate targets the existing **BSP-only, cooperative kernel scheduler**.
Bootstrap calls are allowed; IRQ callbacks and AP calls panic. Contenders
park on a FIFO queue and ownership is handed directly to the next task.
Only queue changes briefly disable interrupts; disk operations retain the
caller's interrupt state. This is not an SMP mutex or kernel-preemption support.

Lock order is VFS, then backend, then transport. Never enter the VFS while
holding a transport lock or another lock needed by the current VFS owner.
One slow operation blocks all filesystem callers, including other mounts;
there is no priority inheritance. Per-volume/inode locks can come later,
once their lifetime and lock-order rules are justified by workloads.

A task is marked `vfs_active` before it queues, until it unlocks. Killing
such a task returns failure (retry later), protecting its stack, descriptors
and buffers. Reaping claims a zombie before closing its descriptors because
close may now sleep; the idle task never performs this blocking cleanup.

Callers must still own their handle/buffer storage until calls return. Do
not free a shared `FILE *` while another task uses it. Separate seek/write
calls are not atomic; `vfs_append` refreshes EOF and writes under one gate,
and kernel stdio append uses it. Directory iteration across several calls
is not a snapshot of concurrent namespace changes.

## Scope and deliberate limits

- Public syscall/stdio entry points and return conventions are unchanged:
  integer operations return `0/-1`, transfers return bytes, directory
  iteration returns positive/zero/negative. Paths are absolute, slash-separated and bounded
  by `VFS_PATH_MAX`. FAT's legacy direct API retains DOS path parsing for
  its existing tests; the VFS does not use that parser.
- Mount names are case-sensitive, even on FAT. Repeated/trailing slashes
  are normalized; dot components in mount names are rejected. Existing TOS
  synthetic mountpoints are supported without requiring a disk entry there;
  ancestor components must still resolve when the mount is accessed.
- FAT geometry and persistence state live in one `fat_volume` record.
  FAT retains one mounted volume. A second VFS mount is rejected before
  changing any FAT state. Do not call legacy `fat_init` or mutate through
  the direct FAT API while a VFS mount is live. Ext2 supports multiple mounts.
- File `size/type/attributes` fields are compatibility snapshots.
  `vfs_file_stat` refreshes them. FAT inode identity comes from the directory
  entry's image offset, not its changing first cluster.
- Removing an open file/directory is rejected; Linux-style deferred deletion
  needs orphan-storage support first. Unmount rejects live handles and child
  mounts. Close/unmount before freeing images.
- Calls are serialized, not parallel across mounts. There is no dentry cache,
  page cache, symlink/rename API, new permission
  enforcement. FAT seek still clamps at EOF; ext2 supports sparse seeks.

## Persistent storage

`drivers/storage/block.h` is the common 512-byte sector interface. A transport
supplies read, write, flush and capacity; callers supply ordinary virtual
buffers. AHCI uses one low physical bounce page, so heap/stack buffers do not
need to be contiguous or DMA-addressable.

Both filesystems mount through that interface, so either can be read from
either transport. `fs/rootfs.c` is the boot-time search that pairs them up: it
walks the transports the drivers discovered and offers each volume to each
registered filesystem until one recognises it, then attaches USB volumes under
the root it found. Adding a filesystem to that search is one table row. The AHCI command builder is shared
by reads, writes, IDENTIFY and FLUSH CACHE EXT. Failed commands disable the
port and stop DMA before releasing buffers; recovery currently needs reboot.

`ext2_mount_device(path, device)` copies a raw volume into an owned cache
(currently capped at 128 MiB). The transport context must outlive the mount.
Existing pointer-based inode/directory algorithms continue to work, while a
dirty bitmap writes only changed blocks. This was chosen over a full page
cache to avoid changing every disk algorithm at once; larger disks need a
bounded block cache next, not a larger arbitrary allocation limit.

Mutations synchronously persist an unclean superblock and flush it, write the
changed blocks and flush, then persist the clean superblock and flush again.
This is **not journaling**: an interrupted update needs external `e2fsck`.
Unclean device volumes are refused. An I/O failure preserves dirty blocks
and blocks new mutations; explicit `vfs_file_sync`, `vfs_sync_all` or unmount
can retry. A failed operation may already have changed the cache/disk, so it
does not promise rollback. Shutdown/reboot refuse to proceed after sync fails.
Only the primary superblock/group descriptors are maintained, not backups.

Ext2 accepts 1/2/4 KiB blocks, direct/single/double/triple-indirect storage,
and files with 32-bit sizes. Journals, unsupported incompatible/read-only
features, extended-attribute inodes, indexed directories, special inode flags,
symlinks and files above 4 GiB are not supported. Unsupported inodes are
refused rather than exposed as ordinary writable files. Use disposable images
until wider corruption/fault and hardware testing is available.

## USB mass storage

The existing `drivers/usb/storage` files have separate responsibilities:

- `bot.c`: CBW/data/CSW framing, tags, lengths, stalls and reset recovery.
- `scsi.c`: inquiry, readiness, capacity, sector reads/writes and cache flush.
- `usb_storage.c`: descriptor validation and the discovered-disk registry.
- `usb_storage.h`: their shared types and the small control/bulk HCD contract.

EHCI supplies synchronous bulk transfers using its existing DMA arena. The
asynchronous schedule is stopped before recycling descriptors; a lock protects
that arena. BOT separately serializes entire commands. No transfer is replayed
automatically after a transport failure. A disconnect permanently invalidates
that device context, preventing a stale mount from writing to a replacement.
Cached reads can still return old data after unplugging; this is not hotplug.

Boot-attached high-speed EHCI SCSI/BOT disks with 512-byte sectors are supported,
one interface and LUN 0 per device, up to eight devices. Raw ext2 and FAT
volumes both mount at `/usb0` etc, because a USB disk is a `block_device` like
any other. There is still no MBR/GPT partition scan, hub support, hotplug
enumeration, xHCI, UAS, READ CAPACITY(16), or non-512-byte sectors, so a stick
with a partition table still will not mount: the filesystem has to start at
LBA 0.

## Verification and extension

Run `make test-fs` for the VFS allocation-failure tests, FAT16/32 tests,
ext2 tests, stdio tests, serialization tests and a read-only `e2fsck` of the
mutated ext2 image. `fat_device_test` covers the device-backed FAT path against
a fake transport, because the QEMU persistence tests put ext2 on their disks and
so never take it. Host filesystem tests require pthreads. The serialization
test runs the real gate and ext2 with a pthread IRQ/scheduler adapter: eight
workers mutate namespaces, append through independent handles and write a
shared cursor. A deterministic queue checks read/close/unmount ordering;
subprocess checks require recursion, unlocked helpers and IRQ/AP entry to
panic. The adapter asserts waiter pinning and bounds waits to ten seconds.
These host tests do not emulate kernel context switches or kill/reap races;
QEMU persistence/system-stress tests provide complementary integration checks.
`tests/vfs_backend_checks.h` runs the same object/lifetime contract against
all three disk layouts. `make kernel` checks kernel integration.

`make test-storage` adds BOT/SCSI wire-protocol and error-recovery tests.
The ext2 device test verifies persistence through fresh mounts, sparse indirect
blocks and write/flush failures. With the ISO and fixtures built, run
`python tests/storage_persistence_test.py --transport ahci` and repeat with
`--transport usb`. These copy generated images to disposable `build/tests`
disks, write a file, restart QEMU completely and read it back. Run `e2fsck -fn`
on `build/tests/ahci-persistence.img` and `usb-persistence.img` afterward.
`python tests/ehci_test.py` checks existing control/periodic HID transfers.

To add a filesystem, implement component-based inode operations and file
operations, supply a root inode from its mount hook, then register its type,
and add it to the table in `fs/rootfs.c` if it should be considered at boot.
Do not add another pathname walker or change syscall callers.

`kernel/fs_old` is an untouched migration reference, excluded from the
kernel build and generated editor compilation database.

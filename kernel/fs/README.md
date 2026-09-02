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
| `stdio.c` | Kernel `FILE *` API over the VFS |
| `fat/fat.c` | Legacy FAT path API and shared component mutations |
| `fat/fat_mount.c` | Volume validation, geometry, cluster chains and persistence |
| `fat/fat_file.c` | File transfer, cursor movement and truncation |
| `fat/fat_directory.c` | Directory cursors, entries and slot allocation |
| `fat/fat_name.c` | Long-name encoding and DOS 8.3 aliases |
| `fat/fat_vfs.c` | Translate FAT entries to VFS objects |
| `fat/fat_internal.h` | Shared FAT layouts, constants and helper declarations |
| `fat/ahci/` | Existing AHCI image loading and write-through |
| `ext2/ext2_mount.c` | Superblock, feature and geometry validation |
| `ext2/ext2_io.c` | Device-backed cache, dirty-block writeback and barriers |
| `ext2/ext2_inode.c` | Inodes, block allocation and indirect blocks |
| `ext2/ext2_dir.c` | Directory entries and component lookup |
| `ext2/ext2_file.c` | Sparse file data transfers |
| `ext2/ext2_vfs.c` | Translate ext2 inodes to VFS objects |

FAT16 and FAT32 use the same directory algorithms. Their differences are
geometry and cluster encoding, so there are no duplicated format operation
tables or one-function selector files. Shared logic uses ordinary functions;
macros are reserved for constants and on-disk layout declarations.

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

## Scope and deliberate limits

- Public syscall/stdio entry points and return conventions are unchanged:
  integer operations return `0/-1`, transfers return bytes, directory
  iteration returns `1/0/-1`. Paths are absolute, slash-separated and bounded
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
- The original serialization requirement remains: this is not a concurrent
  VFS. There is no dentry cache, page cache, symlink/rename API, new permission
  enforcement. FAT seek still clamps at EOF; ext2 supports sparse seeks.

## Persistent storage

`drivers/storage/block.h` is the common 512-byte sector interface. A transport
supplies read, write, flush and capacity; callers supply ordinary virtual
buffers. AHCI uses one low physical bounce page, so heap/stack buffers do not
need to be contiguous or DMA-addressable. The AHCI command builder is shared
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

## Verification and extension

Run `make test-fs` for the VFS allocation-failure tests, FAT16/32 tests,
ext2 tests, stdio tests and a read-only `e2fsck` of the mutated ext2 image.
`tests/vfs_backend_checks.h` runs the same object/lifetime contract against
all three disk layouts. `make kernel` checks kernel integration.

To add a filesystem, implement component-based inode operations and file
operations, supply a root inode from its mount hook, then register its type.
Do not add another pathname walker or change syscall callers.

`kernel/fs_old` is an untouched migration reference, excluded from the
kernel build and generated editor compilation database.

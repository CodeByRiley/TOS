# Filesystem architecture

Kernel callers use `vfs/` and never depend on an on-disk filesystem handle.
The VFS owns mountpoint routing, generic file handles, metadata, directory
entries, and lifecycle dispatch. Mount resolution uses the longest complete
path prefix, so `/mnt/data` wins over `/` while `/mntish` still belongs to the
root mount.

## Backends

`fat/` retains the existing FAT16/FAT32 implementation. `fat_vfs.c` is its
translation layer, and `fat/ahci/` is the optional persistent storage backend.
Keeping AHCI outside the format driver lets FAT host tests use a plain memory
image.

`ext2/` is split by on-disk responsibility:

- `ext2_mount.c` validates superblocks, features, geometry, and group tables.
- `ext2_inode.c` owns inode lookup, bitmaps, block allocation, and indirect
  block trees.
- `ext2_dir.c` owns component lookup, path walking, directory mutation, and
  enumeration.
- `ext2_file.c` transfers file bytes, including sparse reads and writes.
- `ext2_vfs.c` translates those primitives into the generic VFS operations.

The ext2 backend supports revision 0/1 filesystems, 1 KiB through 4 KiB block
sizes, 128-byte or larger inodes, direct/single/double-indirect files, regular
files, and directories. It deliberately rejects journaled or unknown
incompatible features; mounting an ext3/ext4 image as ext2 without replaying
its journal could silently corrupt it.

Currently an ext2 root is supplied as a GRUB memory module. FAT additionally
has AHCI write-through. A future generic block-device layer can replace the
memory-image field in each mount context without changing syscall or stdio
callers.

## Adding another filesystem

Implement a `struct vfs_filesystem` with a probe, mount routine, and operations
table, then register it before calling `vfs_mount_auto`. Backend file state is
stored in `vfs_file.private_data`; release it in the backend's `close` hook.
All IPC-independent kernel and userspace file APIs remain unchanged.

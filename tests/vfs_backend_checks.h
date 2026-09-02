/* One contract suite reused for FAT16, FAT32 and ext2. */
#ifndef VFS_BACKEND_CHECKS_H
#define VFS_BACKEND_CHECKS_H
#include "fs/vfs/vfs.h"
#include <string.h>

static int vfs_backend_checks(int (*check)(int, const char *)) {
    int failed = 0;
#define VFS_CHECK(condition, message) (failed |= check((condition), (message)))
    struct vfs_file first = {0}, second = {0}, empty = {0}, dir = {0};
    struct vfs_stat metadata;
    char data[8] = {0};
    VFS_CHECK(vfs_mkdir("/VFSTEST") == 0, "VFS contract: mkdir");
    VFS_CHECK(vfs_create("/VFSTEST/A", &first) == 0 &&
              vfs_create("/VFSTEST/B", &empty) == 0,
              "VFS contract: create independent empty files");
    uint64_t identity = first.inode;
    VFS_CHECK(first.inode != empty.inode, "VFS contract: empty files have distinct identities");
    VFS_CHECK(vfs_open("/VFSTEST/A", &second) == 0 && first.node == second.node,
              "VFS contract: independent opens share inode");
    VFS_CHECK(vfs_write(&first, "data", 4) == 4 && first.inode == identity,
              "VFS contract: first allocation preserves identity");
    VFS_CHECK(second.position == 0 && vfs_read(&second, data, 4) == 4 &&
              !memcmp(data, "data", 4), "VFS contract: peer observes updated file data");
    VFS_CHECK(vfs_file_stat(&second, &metadata) == 0 && metadata.size == 4,
              "VFS contract: fstat observes shared size");
    VFS_CHECK(vfs_unlink("/VFSTEST/A") < 0 && vfs_unmount("/") < 0,
              "VFS contract: open files protect storage lifetime");
    VFS_CHECK(vfs_truncate(&first) == 0 && first.inode == identity &&
              vfs_file_stat(&second, &metadata) == 0 && metadata.size == 0,
              "VFS contract: truncate keeps identity and updates peers");
    vfs_close(&first); vfs_close(&second); vfs_close(&empty);
    VFS_CHECK(vfs_open("/VFSTEST", &dir) == 0 &&
              vfs_file_stat(&dir, &metadata) == 0 && metadata.type == VFS_NODE_DIRECTORY,
              "VFS contract: directories support open and fstat");
    VFS_CHECK(vfs_read(&dir, data, 1) == 0 && vfs_write(&dir, data, 1) == 0 &&
              vfs_seek(&dir, 0) < 0 && vfs_truncate(&dir) < 0,
              "VFS contract: directory byte operations fail safely");
    VFS_CHECK(vfs_stat("/VFSTEST/A/", &metadata) < 0 &&
              vfs_stat("/VFSTEST/A/..", &metadata) < 0 &&
              vfs_stat("/MISSING/../VFSTEST", &metadata) < 0,
              "VFS contract: each traversed component must be a directory");
    VFS_CHECK(vfs_stat("//VFSTEST/./../VFSTEST/A", &metadata) == 0,
              "VFS contract: dot components and repeated separators");
    VFS_CHECK(vfs_create("/VFSTEST/C/", &first) < 0 &&
              vfs_unlink("/VFSTEST/A/") < 0 && vfs_rmdir("/VFSTEST/.") < 0,
              "VFS contract: mutation rejects inappropriate trailing components");
    VFS_CHECK(vfs_unlink("/VFSTEST/A") == 0 && vfs_unlink("/VFSTEST/B") == 0,
              "VFS contract: closed files can be removed");
    VFS_CHECK(vfs_rmdir("/VFSTEST") < 0, "VFS contract: open directory cannot be removed");
    vfs_close(&dir);
    VFS_CHECK(vfs_rmdir("/VFSTEST") == 0, "VFS contract: remove closed empty directory");
#undef VFS_CHECK
    return failed;
}
#endif

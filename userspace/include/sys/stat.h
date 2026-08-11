/* userspace/include/sys/stat.h — file-metadata structs + mode bits.
 *
 * stat()/fstat() are backed by SYS_STAT / SYS_FSTAT and implemented in
 * lib/stat.c, which expands the kernel's compact struct stat_user into
 * the POSIX shape below; mkdir() is backed by the FAT directory syscall.
 * Fields FAT cannot answer for are documented in lib/stat.c — notably the
 * timestamps, which are always 0.
 *
 * Mode bits follow the standard Unix octal encoding so ported code works
 * unchanged.
 */
#ifndef SYS_STAT_H
#define SYS_STAT_H

#include "types.h"
#include <stdint.h>

struct stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint64_t st_rdev;

    off_t    st_size;
    time_t   st_atime;
    time_t   st_mtime;
    time_t   st_ctime;

    mode_t   st_mode;
    uid_t    st_uid;
    gid_t    st_gid;
};

/* File-type bits in st_mode. */
#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFBLK  0060000
#define S_IFIFO  0010000
#define S_IFLNK  0120000

#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)

/* Permission bits. */
#define S_IRUSR  0400
#define S_IWUSR  0200
#define S_IXUSR  0100
#define S_IRGRP  0040
#define S_IWGRP  0020
#define S_IXGRP  0010
#define S_IROTH  0004
#define S_IWOTH  0002
#define S_IXOTH  0001

int stat(const char *path, struct stat *buf);
int fstat(int fd, struct stat *buf);
int mkdir(const char *path, mode_t mode);

#endif

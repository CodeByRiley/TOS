/* userspace/include/errno.h , POSIX-style error codes. */
#ifndef ERRNO_H
#define ERRNO_H

#include "utilities/types.h"

extern int errno;

/*
 * If compiling under C23 or newer, use the typed enum.
 * Otherwise, fall back to standard C99/C17 untyped enum for DOOM.
 */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
enum errornum : uchar {
#else
enum errornum {
#endif
    EPERM        = 1,
    ENOENT       = 2,
    ESRCH        = 3,
    EINTR        = 4,
    EIO          = 5,
    ENXIO        = 6,
    E2BIG        = 7,
    ENOEXEC      = 8,
    EBADF        = 9,
    ECHILD       = 10,
    EAGAIN       = 11,
    ENOMEM       = 12,
    EACCES       = 13,
    EFAULT       = 14,
    ENOTBLK      = 15,
    EBUSY        = 16,
    EEXIST       = 17,
    EXDEV        = 18,
    ENODEV       = 19,
    ENOTDIR      = 20,
    EISDIR       = 21,
    EINVAL       = 22,
    ENFILE       = 23,
    EMFILE       = 24,
    ENOTTY       = 25,
    ETXTBSY      = 26,
    EFBIG        = 27,
    ENOSPC       = 28,
    ESPIPE       = 29,
    EROFS        = 30,
    EMLINK       = 31,
    EPIPE        = 32,
    EDOM         = 33,
    ERANGE       = 34,
    EDEADLK      = 35,
    ENAMETOOLONG = 36,
    ENOLCK       = 37,
    ENOSYS       = 38,
    ENOTEMPTY    = 39,
    ELOOP        = 40,
};

#endif

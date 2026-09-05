/* kernel/utilities/errno.h , error numbers returned across the syscall ABI.
 *
 * A syscall that fails returns the NEGATED code, which is the Linux
 * convention and the reason these numbers are the Linux ones rather than
 * anything of our own: musl's __syscall_ret turns a return in [-4095, -1]
 * into errno = -ret and a -1 result without knowing anything about TOS, so a
 * musl-linked caller gets working errno and strerror() for free.
 *
 * The same numbers appear in userspace/include/errno.h for the hand-rolled
 * libc, and in musl's own headers. Nothing includes another's copy , the
 * kernel cannot reach userspace headers and musl must not see ours , so what
 * keeps the three agreeing is that all of them are the POSIX/Linux values.
 * Add a code here only with the number Linux uses for it.
 *
 * Only the codes the kernel actually returns are listed. A syscall that has
 * not been converted still returns a bare -1, which reads as -EPERM; that is
 * wrong but harmless, and is why new code should return a real code.
 */
#ifndef KERNEL_ERRNO_H
#define KERNEL_ERRNO_H

#define EPERM        1  /* Operation not permitted */
#define ENOENT       2  /* No such file or directory */
#define ESRCH        3  /* No such process */
#define EIO          5  /* Input/output error */
#define ENXIO        6  /* No such device or address */
#define EBADF        9  /* Bad file descriptor */
#define EAGAIN      11  /* Try again; also a futex whose value already moved */
#define ENOMEM      12  /* Out of memory, or a fixed kernel table is full */
#define EFAULT      14  /* Bad address supplied by userspace */
#define EBUSY       16  /* Device or resource busy */
#define EEXIST      17  /* Already exists */
#define ENODEV      19  /* No such device, or no filesystem of that name */
#define ENOTDIR     20  /* Not a directory */
#define EISDIR      21  /* Is a directory */
#define EINVAL      22  /* Invalid argument, including a bad superblock */
#define EMFILE      24  /* This process has no free descriptor */
#define ENOTTY      25  /* Inappropriate ioctl for this device */
#define ERANGE      34  /* Result too large for the caller's buffer */
#define ENAMETOOLONG 36 /* Name too long for the buffer it must fit */
#define ENOSYS      38  /* Function not implemented */

/* Socket codes live above the range userspace/include/errno.h enumerates, so
 * they are mirrored there alongside these. musl already knows them. */
#define ENOTSOCK        88 /* Descriptor is not a socket */
#define EPROTONOSUPPORT 93 /* Protocol or socket type not supported */
#define EAFNOSUPPORT    97 /* Address family not supported */
#define EADDRINUSE      98 /* Address or port already in use */

#endif

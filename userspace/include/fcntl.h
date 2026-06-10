/* userspace/include/fcntl.h — open() flag constants.
 *
 * The open() prototype itself is declared in syscall.h so the same
 * signature is shared with the syscall wrapper. We only expose flag
 * bits here. Values match the Linux ABI.
 */
#ifndef FCNTL_H
#define FCNTL_H

#define O_RDONLY   0
#define O_WRONLY   1
#define O_RDWR     2
#define O_CREAT    0x40
#define O_EXCL     0x80
#define O_TRUNC    0x200
#define O_APPEND   0x400
#define O_NONBLOCK 0x800

#endif

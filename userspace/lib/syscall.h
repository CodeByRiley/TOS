#ifndef USER_SYSCALL_H
#define USER_SYSCALL_H

#include <stddef.h>
#include <stdint.h>

#define SYS_READ    0
#define SYS_WRITE   1
#define SYS_OPEN    2
#define SYS_CLOSE   3
#define SYS_LSEEK   8
#define SYS_MMAP    9
#define SYS_READDIR 10
#define SYS_EXIT    60
#define SYS_FB_INFO    100
#define SYS_FB_MAP     101
#define SYS_KBD_POLL   102
#define SYS_GET_TICKS  103
#define SYS_EXEC       104
#define SYS_UNLINK     87


#define SYS_SHUTDOWN   888
#define SYS_REBOOT     887

struct fb_info { uint64_t width, height, pitch, bpp; };


long syscall0(long n);
long syscall1(long n, long a);
long syscall2(long n, long a, long b);
long syscall3(long n, long a, long b, long c);
long syscall6(long n, long a, long b, long c, long d, long e, long f);

long write(int fd, const void *buf, size_t n);
long read(int fd, void *buf, size_t n);
long open(const char *path, int flags);
long close(int fd);
long lseek(int fd, long off, int whence);
long readdir(unsigned *index, char *buf, size_t n);
long unlink(const char *path);
void exit(int code);

long  fb_info(struct fb_info *out);
void *fb_map(void);
long  kbd_poll(int *pressed, uint16_t *key);
long  get_ticks(void);
long  exec(const char *path, char *const argv[]);
long  sys_shutdown(int time, const char *reason);
long  sys_reboot(int time);

#endif

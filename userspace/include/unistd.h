#ifndef UNISTD_H
#define UNISTD_H

#include <stddef.h>
#include "../lib/syscall.h"

/* read/write/open/close/lseek/unlink declared in syscall.h */

int    isatty(int fd);
int    chdir(const char *path);
char  *getcwd(char *buf, size_t size);
unsigned int sleep(unsigned int seconds);
int    usleep(unsigned int us);

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#endif

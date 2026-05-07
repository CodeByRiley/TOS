#include "../include/unistd.h"
#include "syscall.h"

int isatty(int fd)                       { (void)fd; return 0; }
int chdir(const char *p)                { (void)p; return -1; }
char *getcwd(char *buf, size_t size)    { if (buf && size) buf[0] = 0; return buf; }

unsigned int sleep(unsigned int s) {
    long target = get_ticks() + (long)s * 1000;
    while (get_ticks() < target) { }
    return 0;
}

int usleep(unsigned int us) {
    long target = get_ticks() + (long)(us / 1000);
    while (get_ticks() < target) { }
    return 0;
}

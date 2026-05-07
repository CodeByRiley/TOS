#include "../include/sys/stat.h"

int stat(const char *path, struct stat *buf)  { (void)path; (void)buf; return -1; }
int fstat(int fd, struct stat *buf)            { (void)fd; (void)buf; return -1; }
int mkdir(const char *path, mode_t mode)       { (void)path; (void)mode; return -1; }

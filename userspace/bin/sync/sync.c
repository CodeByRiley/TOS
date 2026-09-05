/* Flush every mounted filesystem and its transport cache. */
#include <stdio.h>
#include <lib/syscall.h>
int main(int argc, char **argv) {
    (void)argv;
    if (argc != 1) { printf("usage: sync\n"); return 2; }
    if (fs_sync() != 0) { printf("sync: filesystem flush failed\n"); return 1; }
    return 0;
}

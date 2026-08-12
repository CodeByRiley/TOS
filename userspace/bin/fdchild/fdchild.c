#include "../../include/stdio.h"
#include "../../lib/syscall.h"

int main(void) {
    int fd = (int)open("readme.txt", 0);
    printf("fdchild: open returned fd=%d\n", fd);

    if (fd >= 0)
        close(fd);

    return fd == 3 ? 0 : 1;
}

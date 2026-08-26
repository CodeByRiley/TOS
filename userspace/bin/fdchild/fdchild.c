#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    int fd = open("readme.txt", O_RDONLY);
    printf("fdchild: open returned fd=%d\n", fd);

    if (fd >= 0)
        close(fd);

    return fd == 3 ? 0 : 1;
}

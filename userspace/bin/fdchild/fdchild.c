#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <lib/app_info.h>

APP_INFO(APP_TYPE_CLI, "fdchild");

int main(void) {
    int fd = open("readme.txt", O_RDONLY);
    printf("fdchild: open returned fd=%d\n", fd);

    if (fd >= 0)
        close(fd);

    return fd == 3 ? 0 : 1;
}

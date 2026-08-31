/* userspace/bin/cat/cat.c , dump one or more files to stdout.
 *
 * The first binary that was built against musl rather than userspace/lib,
 * and still the smallest one that touches startup, stdio and the syscall
 * layer at once , so it stays the first thing to check when a musl or
 * kernel change breaks userspace. Uses standard headers only: no
 * <lib/syscall.h>, no hand-declared externs.
 */
#include <stdio.h>
#include <unistd.h>
#include <lib/app_info.h>

APP_INFO(APP_TYPE_CLI, "cat");
static int cat_one(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        printf("cat: %s: open failed\n", path);
        return 1;
    }

    char buf[256];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (write(1, buf, n) < 0)
            break;
    }
    fclose(fp);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: cat FILE...\n");
        return 1;
    }

    int failed = 0;
    for (int i = 1; i < argc; i++)
        failed |= cat_one(argv[i]);
    return failed ? 1 : 0;
}

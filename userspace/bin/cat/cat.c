/* userspace/bin/cat/cat.c — dump one or more files to stdout.
 *
 * First binary built against musl rather than userspace/lib. It uses only
 * standard headers: no <lib/syscall.h>, no hand-declared externs. The
 * hand-rolled libc is still what every other binary links, so this one is
 * the canary for the migration — if musl's startup, stdio, or syscall layer
 * is wrong on TOS, `cat` is where it shows up first.
 */
#include <stdio.h>
#include <unistd.h>

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

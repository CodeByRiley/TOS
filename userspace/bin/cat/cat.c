/* userspace/bin/cat/cat.c — dump one or more files to stdout. */
#include <lib/syscall.h>
#include <stddef.h>

extern void *fopen(const char *, const char *);
extern size_t fread(void *, size_t, size_t, void *);
extern int    fclose(void *);
extern int    printf(const char *, ...);

static int cat_one(const char *path) {
    void *fp = fopen(path, "rb");
    if (!fp) {
        printf("cat: %s: open failed\n", path);
        return 1;
    }

    char buf[256];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        write(1, buf, n);
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

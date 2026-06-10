/* userspace/bin/ls/ls.c — list filesystem entries.
 *
 * Walks the kernel's SYS_READDIR enumerator. Only the root directory is
 * supported today since the FAT layer doesn't expose subdirs to userspace
 * yet; any other path prints an explanatory error.
 *
 * The readdir buffer holds a packed sequence of NUL-terminated names; the
 * inner loop walks past each name with strlen() to find the next.
 */
#include "../../lib/syscall.h"
#include "../../include/string.h"

extern int printf(const char *, ...);
extern int strcmp(const char *, const char *);
extern size_t strlen(const char *);

#define BUF_SIZE 256

/* Print every entry under `path`. Returns silently if path is unsupported. */
void list_directory(const char *path) {
    if (strcmp(path, ".") != 0 && strcmp(path, "/") != 0) {
        printf("ls: only the root directory is supported: %s\n", path);
        return;
    }

    unsigned index = 0;
    char buf[BUF_SIZE];
    long bytes_read;

    while ((bytes_read = readdir(&index, buf, sizeof(buf))) > 0) {
        for (long i = 0; i < bytes_read; i++) {
            if (buf[i] == '\0') continue;
            printf("%s\n", &buf[i]);
            i += strlen(&buf[i]);
        }
    }
}

int main(int argc, char *argv[]) {
    const char *path = (argc > 1) ? argv[1] : ".";
    list_directory(path);
    return 0;
}

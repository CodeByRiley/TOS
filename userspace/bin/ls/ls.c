/* userspace/bin/ls/ls.c — list filesystem entries.
 *
 * Walks a root-relative FAT directory. Directory names returned by the
 * kernel carry a trailing slash.
 *
 * The readdir buffer holds a packed sequence of NUL-terminated names; the
 * inner loop walks past each name with strlen() to find the next.
 */
#include <lib/syscall.h>
#include <string.h>

extern int printf(const char *, ...);
extern size_t strlen(const char *);

#define BUF_SIZE 1024

/* Print every entry under path. */
void list_directory(const char *path) {
    unsigned index = 0;
    char buf[BUF_SIZE];
    long bytes_read;

    while ((bytes_read = readdir_path(path, &index, buf, sizeof(buf))) > 0) {
        for (long i = 0; i < bytes_read; i++) {
            if (buf[i] == '\0') continue;
            printf("%s\n", &buf[i]);
            i += strlen(&buf[i]);
        }
    }
    if (bytes_read < 0)
        printf("ls: cannot open directory: %s\n", path);
}

int main(int argc, char *argv[]) {
		const char *path = (argc > 1) ? argv[1] : ".";
    list_directory(path);
    return 0;
}

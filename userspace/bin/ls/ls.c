#include "../../lib/syscall.h"
#include "../../include/string.h"

extern int printf(const char *, ...);
extern int strcmp(const char *, const char *);
extern size_t strlen(const char *);

#define BUF_SIZE 256

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
            if (buf[i] == '\0') continue;  // Ignore empty entries
            printf("%s\n", &buf[i]);     // Print the entry name
            i += strlen(&buf[i]);         // Skip to the end of the current entry
        }
    }
}

int main(int argc, char *argv[]) {
    const char *path = (argc > 1) ? argv[1] : ".";
    list_directory(path);
    return 0;
}

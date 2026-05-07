#include "../../lib/syscall.h"
#include <stddef.h>

extern void *fopen(const char *, const char *);
extern size_t fread(void *, size_t, size_t, void *);
extern int    fclose(void *);
extern int    printf(const char *, ...);

int main(void) {
    void *fp = fopen("README.TXT", "rb");
    if (!fp) { printf("cat: open failed\n"); return 1; }

    char buf[256];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        write(1, buf, n);
    }
    fclose(fp);
    printf("[cat done, malloc/printf/fread work]\n");
    return 0;
}

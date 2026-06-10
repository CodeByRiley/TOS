/* userspace/bin/cat/cat.c — dump a known file to stdout.
 *
 * Hard-coded to README.TXT today because there's no argv-driven open path
 * exercised in this binary yet. Doubles as an integration smoke test for
 * fopen/fread/fclose + printf.
 */
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

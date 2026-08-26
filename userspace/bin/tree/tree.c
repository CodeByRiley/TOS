#include <lib/syscall.h>
#include <lib/console.h>
#include <utilities/types.h>
#include <stdarg.h>

#define MAX_ENTRIES 64
#define NAME_MAX_LEN 128

extern int    vsnprintf(char *, size_t, const char *, va_list);
extern void  *malloc(size_t);
extern void   free(void *);
extern size_t strlen(const char *);
extern int    strcmp(const char *, const char *);
extern char  *strcpy(char *, const char *);
extern char  *strcat(char *, const char *);
extern void  *memset(void *, int, size_t);

static int printf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    console_write(buf, r);
    return r;
}

static void print_tree(const char *path, const char *prefix) {
    /* Buffer entries to identify the final branch. */
    char (*names)[NAME_MAX_LEN] = malloc(MAX_ENTRIES * NAME_MAX_LEN);
    int  *is_dir = malloc(MAX_ENTRIES * sizeof(int));

    if (!names || !is_dir) {
        printf("tree: out of memory\n");
        if (names) free(names);
        if (is_dir) free(is_dir);
        return;
    }

    int count = 0;
    uint32_t idx = 0;
    char dbuf[1024];

    // 1. Read all entries in the current directory
    while (count < MAX_ENTRIES) {
        long n = readdir_path(path, &idx, dbuf, sizeof(dbuf));
        if (n <= 0) break;

        long off = 0;
        while (off < n && count < MAX_ENTRIES) {
            const char *name = dbuf + off;
            int nl = (int)strlen(name);

            if (nl > 0 && strcmp(name, ".") != 0 && strcmp(name, "..") != 0) {
                strcpy(names[count], name);

                // Construct full path to stat the entry
                char full_path[256];
                strcpy(full_path, path);
                if (path[strlen(path) - 1] != '/') strcat(full_path, "/");
                strcat(full_path, name);

                struct stat_user st;
                if (stat_raw(full_path, &st) == 0) {
                    is_dir[count] = (st.type == STAT_TYPE_DIR);
                } else {
                    is_dir[count] = 0; // Default to file if stat fails
                }
                count++;
            }
            off += nl + 1;
        }
    }

    // 2. Print entries and recurse into directories
    for (int i = 0; i < count; i++) {
        int is_last = (i == count - 1);

        printf("%s%s%s\n", prefix, is_last ? "`-- " : "|-- ", names[i]);

        if (is_dir[i]) {
            // Build new path for recursion
            char new_path[256];
            strcpy(new_path, path);
            if (path[strlen(path) - 1] != '/') strcat(new_path, "/");
            strcat(new_path, names[i]);

            // Build new prefix for children
            char new_prefix[256];
            strcpy(new_prefix, prefix);
            strcat(new_prefix, is_last ? "    " : "|   ");

            print_tree(new_path, new_prefix);
        }
    }

    free(names);
    free(is_dir);
}

int main(int argc, char **argv) {
    const char *start_dir = ".";
    if (argc > 1) {
        start_dir = argv[1];
    }

    printf("%s\n", start_dir);
    print_tree(start_dir, "");

    return 0;
}

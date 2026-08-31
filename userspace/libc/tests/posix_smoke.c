#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

static int fail(const char *what) {
    printf("posix-smoke: FAIL %s errno=%d\n", what, errno);
    return 1;
}

int main(void) {
    printf("posix-smoke: start\n");

    struct utsname uts;
    if (uname(&uts) != 0) return fail("uname");
    if (strcmp(uts.sysname, "TOS") != 0 ||
        strcmp(uts.machine, "x86_64") != 0)
        return fail("uname-shape");

    struct sigaction old_pipe;
    struct sigaction ignore_pipe;
    struct sigaction readback;
    memset(&ignore_pipe, 0, sizeof(ignore_pipe));
    ignore_pipe.sa_handler = SIG_IGN;
    if (sigemptyset(&ignore_pipe.sa_mask) != 0)
        return fail("sigemptyset");
    if (sigaction(SIGPIPE, &ignore_pipe, &old_pipe) != 0)
        return fail("sigaction-set");
    if (sigaction(SIGPIPE, NULL, &readback) != 0 ||
        readback.sa_handler != SIG_IGN)
        return fail("sigaction-readback");
    if (sigaction(SIGPIPE, &old_pipe, NULL) != 0)
        return fail("sigaction-restore");

    char *heap = malloc(8192);
    if (!heap) return fail("malloc");
    strcpy(heap, "heap-ok");
    if (strcmp(heap, "heap-ok") != 0) return fail("heap-contents");
    free(heap);

    FILE *fp = fopen("/readme.txt", "r");
    if (!fp) return fail("fopen");
    char buf[64];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    if (n == 0) return fail("fread");
    buf[n] = 0;
    printf("posix-smoke: read %zu bytes\n", n);

    struct stat st;
    if (stat("/readme.txt", &st) != 0) return fail("stat");
    if (!S_ISREG(st.st_mode) || st.st_size <= 0) return fail("stat-shape");
    printf("posix-smoke: stat size=%lld\n", (long long)st.st_size);

    char cwd[128];
    if (!getcwd(cwd, sizeof(cwd))) return fail("getcwd-root");
    if (chdir("/holyd") != 0) return fail("chdir");
    if (!getcwd(cwd, sizeof(cwd))) return fail("getcwd-holyd");
    printf("posix-smoke: cwd=%s\n", cwd);
    if (chdir("/") != 0) return fail("chdir-root");

    DIR *dir = opendir("/");
    if (!dir) return fail("opendir");
    int saw_readme = 0;
    int saw_usr = 0;
    int entries = 0;
    for (;;) {
        struct dirent *de = readdir(dir);
        if (!de) break;
        entries++;
        if (strcmp(de->d_name, "readme.txt") == 0) saw_readme = 1;
        if (strcmp(de->d_name, "usr") == 0) saw_usr = 1;
    }
    closedir(dir);
    if (!saw_readme || !saw_usr || entries == 0) return fail("readdir");
    printf("posix-smoke: dir entries=%d\n", entries);

    void *map = mmap(0, 8192, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANON, -1, 0);
    if (map == MAP_FAILED) return fail("mmap");
    strcpy((char *)map, "mmap-ok");
    if (strcmp((char *)map, "mmap-ok") != 0) return fail("mmap-contents");
    if (munmap(map, 8192) != 0) return fail("munmap");

    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return fail("clock_gettime");
    printf("posix-smoke: clock=%lld.%09ld\n",
           (long long)ts.tv_sec, ts.tv_nsec);

    if (!isatty(1)) return fail("isatty");
    struct pollfd pfd = { .fd = 1, .events = POLLOUT };
    if (poll(&pfd, 1, 0) != 1 || !(pfd.revents & POLLOUT))
        return fail("poll");

    printf("posix-smoke: PASS\n");
    return 0;
}

/* userspace/bin/vmtest/vmtest.c — mmap / mprotect / munmap / stat tests.
 *
 * Every case here checks a return value. None of them deliberately
 * violate a mapping: an unhandled user page fault currently panics the
 * whole kernel (idt.c has no per-task fault path), so a test that writes
 * to a read-only page would take the machine down rather than fail. Once
 * user faults kill the faulting task instead, the two cases marked
 * ENFORCEMENT below become worth writing.
 */
#include <lib/syscall.h>

extern int printf(const char *, ...);

static int failures = 0;

static void check(const char *what, int ok) {
    printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
    if (!ok) failures++;
}

/* A PE64 image's default ImageBase. Deliberately outside the mmap arena:
 * this is the case MAP_FIXED exists for. */
#define PE_IMAGE_BASE 0x0000000140000000ULL

static void test_anon(void) {
    printf("anonymous mmap\n");

    size_t len = 4096 * 4;
    unsigned char *p = mmap(0, len, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS);
    check("mmap returns a mapping", p != MAP_FAILED);
    if (p == MAP_FAILED) return;

    check("page-aligned", ((unsigned long)p & 4095) == 0);

    int zeroed = 1;
    for (size_t i = 0; i < len; i++) if (p[i]) { zeroed = 0; break; }
    check("zero-filled", zeroed);

    for (size_t i = 0; i < len; i++) p[i] = (unsigned char)(i & 0xFF);
    int held = 1;
    for (size_t i = 0; i < len; i++)
        if (p[i] != (unsigned char)(i & 0xFF)) { held = 0; break; }
    check("readback across all pages", held);

    check("munmap", munmap(p, len) == 0);

    /* The freed range goes on the hole list, so an identical request has
     * to come back to the same address instead of walking the arena. */
    void *again = mmap(0, len, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS);
    check("freed VA is reused", again == (void *)p);
    if (again != MAP_FAILED) munmap(again, len);
}

static void test_fixed(void) {
    printf("MAP_FIXED\n");

    size_t len = 4096 * 2;
    void *want = (void *)PE_IMAGE_BASE;
    void *got = mmap(want, len, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED);
    check("maps at a PE ImageBase", got == want);
    if (got != want) return;

    volatile unsigned long *probe = (unsigned long *)got;
    *probe = 0xC0FFEE;
    check("writable at the fixed address", *probe == 0xC0FFEE);

    /* Second claim on a live range must be refused, not silently take it:
     * a loader reads the failure as "relocate instead". */
    void *dup = mmap(want, len, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED);
    check("refuses an occupied range", dup == MAP_FAILED);

    check("unaligned address rejected",
          mmap((void *)(PE_IMAGE_BASE + 0x800), len, PROT_READ,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED) == MAP_FAILED);

    check("munmap fixed range", munmap(got, len) == 0);

    /* Now that it is free, the same address must be claimable again. */
    void *retry = mmap(want, len, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED);
    check("re-maps after unmap", retry == want);
    if (retry != MAP_FAILED) munmap(retry, len);
}

static void test_prot(void) {
    printf("mprotect\n");

    size_t len = 4096 * 3;
    unsigned char *p = mmap(0, len, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS);
    if (p == MAP_FAILED) { check("setup mapping", 0); return; }

    p[0] = 0x42;

    /* The sequence a loader runs: write section bytes, then drop write
     * and add execute. */
    check("RW -> RX", mprotect(p, len, PROT_READ | PROT_EXEC) == 0);
    check("still readable", p[0] == 0x42);
    check("RX -> R", mprotect(p, len, PROT_READ) == 0);
    check("R -> RW", mprotect(p, len, PROT_READ | PROT_WRITE) == 0);

    check("partial range", mprotect(p + 4096, 4096, PROT_READ) == 0);
    check("restore partial", mprotect(p + 4096, 4096,
                                      PROT_READ | PROT_WRITE) == 0);

    check("PROT_NONE rejected", mprotect(p, len, PROT_NONE) != 0);
    check("write-only rejected", mprotect(p, len, PROT_WRITE) != 0);
    check("unaligned rejected", mprotect(p + 1, len, PROT_READ) != 0);
    check("unmapped range rejected",
          mprotect((void *)(PE_IMAGE_BASE + 0x10000000), 4096,
                   PROT_READ) != 0);

    /* ENFORCEMENT: after PROT_READ, `p[0] = 1` must kill this process and
     * leave the kernel running. Not tested — today it panics the kernel. */

    check("cleanup", munmap(p, len) == 0);
}

static void test_bad_args(void) {
    printf("argument validation\n");

    check("zero length", mmap(0, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS)
                             == MAP_FAILED);
    check("no PROT_READ", mmap(0, 4096, PROT_EXEC,
                               MAP_PRIVATE | MAP_ANONYMOUS) == MAP_FAILED);
    check("bogus prot bits", mmap(0, 4096, 0x40,
                                  MAP_PRIVATE | MAP_ANONYMOUS) == MAP_FAILED);
    check("kernel-half address refused",
          mmap((void *)0xFFFF800000000000ULL, 4096, PROT_READ,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED) == MAP_FAILED);
    check("null page refused",
          mmap((void *)0, 4096, PROT_READ,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED) == MAP_FAILED);
    check("munmap unaligned", munmap((void *)1, 4096) != 0);
}

static void test_stat(void) {
    printf("stat / fstat\n");

    struct stat_user st;

    check("stat missing file fails", stat_raw("/NOPE.XYZ", &st) != 0);

    check("stat root", stat_raw("/", &st) == 0);
    check("root is a directory", st.type == STAT_TYPE_DIR);

    if (stat_raw("/bin/sh.elf", &st) == 0) {
        check("stat a file", 1);
        check("file is not a directory", st.type == STAT_TYPE_FILE);
        check("file has a size", st.size > 0);

        unsigned long stat_size = st.size;
        long fd = open("/bin/sh.elf", 0);
        check("open the same file", fd >= 0);
        if (fd >= 0) {
            check("fstat", fstat_raw((int)fd, &st) == 0);
            check("fstat size matches stat", st.size == stat_size);

            long end = lseek((int)fd, 0, 2);
            check("size matches lseek to end", (unsigned long)end == stat_size);
            close((int)fd);
        }
    } else {
        check("stat a file (/bin/sh.elf present?)", 0);
    }

    check("fstat on a closed fd fails", fstat_raw(9, &st) != 0);
}

/* shmem_unshare takes a pid and an address in THAT process's space, so a
 * missing range check would make it an arbitrary-unmap primitive against
 * any process — including this one, and including page tables the kernel
 * shares with every process.
 *
 * The two cases that point it at our own memory are deliberately
 * destructive if the guard regresses: the page they name is one we read
 * back immediately afterwards, so a kernel that honoured the request
 * would fault us here rather than let the test pass quietly. With no
 * per-task fault handling that shows up as a hung boot, which is loud
 * enough to notice and better than a silent pass. */
static void test_shmem_unshare(void) {
    printf("shmem_unshare rejections\n");

    long self = get_pid();
    check("get_pid for self", self > 0);

    unsigned char *page = mmap(0, 4096, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS);
    if (page == MAP_FAILED) { check("setup mapping", 0); return; }
    page[0] = 0x5A;

    unsigned long long pv = (unsigned long long)(unsigned long)page;

    check("zero pages rejected",  shmem_unshare((int)self, pv, 0) != 0);
    check("negative pages rejected", shmem_unshare((int)self, pv, -1) != 0);
    check("oversized page count rejected",
          shmem_unshare((int)self, 0x80000000ull, 1 << 20) != 0);
    check("unaligned address rejected",
          shmem_unshare((int)self, 0x80000800ull, 1) != 0);
    check("unknown pid rejected", shmem_unshare(31337, 0x80000000ull, 1) != 0);
    check("pid 0 rejected", shmem_unshare(0, 0x80000000ull, 1) != 0);

    /* Below the shmem arena: our own mmap'd page. */
    check("address below the shmem arena rejected",
          shmem_unshare((int)self, pv, 1) != 0);
    check("that page is still mapped", page[0] == 0x5A);

    /* The kernel half, whose page tables every process shares. Nothing
     * here can fault us — a kernel that obeyed would corrupt itself
     * silently — so the return value is the only evidence available. */
    check("kernel-half address rejected",
          shmem_unshare((int)self, 0xFFFF800000000000ull, 1) != 0);

    /* Inside the arena but not actually shared: nothing to remove, and
     * removing nothing is not an error. */
    check("unshared page in range is a no-op",
          shmem_unshare((int)self, 0x80000000ull, 1) == 0);

    /* Still here, still running, still own our memory. */
    page[0] = 0xA5;
    check("still executing afterwards", page[0] == 0xA5);
    munmap(page, 4096);
}

int main(void) {
    printf("vmtest: memory + metadata syscalls\n\n");

    test_anon();
    test_fixed();
    test_prot();
    test_bad_args();
    test_stat();
    test_shmem_unshare();

    if (failures == 0) printf("\nall checks passed\n");
    else               printf("\n%d check(s) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}

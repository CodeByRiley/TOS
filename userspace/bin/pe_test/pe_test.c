/* userspace/bin/pe_test/pe_test.c , freestanding PE32+ smoke test.
 *
 * Built by mingw into a PE image based at 0x140000000, the default for a
 * 64-bit .exe. Its whole job is to prove the kernel's PE loader mapped an
 * image far above every ELF arena and jumped into it correctly.
 *
 * Nothing from lib/ is linked in. Those objects are ELF and mingw's ld
 * will not put them in a PE image, so this file carries its own entry
 * point, syscall stubs and formatting. Only the SYS_* numbers are shared,
 * pulled in from lib/syscall.h.
 *
 * Two ABIs meet here. mingw compiles to the Microsoft x64 convention
 * (args in rcx/rdx/r8/r9) while TOS syscalls follow the SysV/Linux one
 * (rdi/rsi/rdx/r10/r8/r9, number in rax). The stubs below pin registers
 * explicitly so the compiler emits the moves; do not "simplify" them into
 * a plain call.
 *
 * Nor into `long`. mingw is LLP64, so `long` is FOUR bytes here while the
 * kernel , built LP64 , reads eight. A pointer passed as long arrives
 * truncated to its low 32 bits, which for an image based at 0x140000000
 * means the kernel dereferences an address 4 GiB away from the one meant.
 * Everything crossing the syscall boundary is `sarg`, which is 64-bit
 * under both models.
 */
#include <lib/syscall.h>

typedef long long          sarg;
typedef unsigned long long uarg;

#define ARG(p) ((sarg)(uarg)(__UINTPTR_TYPE__)(p))

/* --- syscall stubs ------------------------------------------------------
 * syscall clobbers rcx (return rip) and r11 (saved rflags). */

static sarg sys1(sarg n, sarg a) {
    sarg ret;
    __asm__ volatile ("syscall"
                      : "=a"(ret)
                      : "a"(n), "D"(a)
                      : "rcx", "r11", "memory");
    return ret;
}

static sarg sys3(sarg n, sarg a, sarg b, sarg c) {
    sarg ret;
    __asm__ volatile ("syscall"
                      : "=a"(ret)
                      : "a"(n), "D"(a), "S"(b), "d"(c)
                      : "rcx", "r11", "memory");
    return ret;
}

static sarg sys4(sarg n, sarg a, sarg b, sarg c, sarg d) {
    sarg ret;
    register sarg r10 __asm__("r10") = d;
    __asm__ volatile ("syscall"
                      : "=a"(ret)
                      : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
                      : "rcx", "r11", "memory");
    return ret;
}

/* fd 1 is mirrored to the serial port by the kernel, which is the only
 * way this output is readable from outside a running VM. */
static void put(const char *s) {
    sarg n = 0;
    while (s[n]) n++;
    sys3(SYS_WRITE, 1, ARG(s), n);
}

static void put_hex(unsigned long long v) {
    static const char digits[] = "0123456789abcdef";
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; i++)
        buf[2 + i] = digits[(v >> ((15 - i) * 4)) & 0xF];
    buf[18] = 0;
    put(buf);
}

static void report(const char *what, int ok) {
    put(ok ? "  ok   " : "  FAIL ");
    put(what);
    put("\n");
}

/* --- section-placement probes -------------------------------------------
 * One object per section so the test can tell whether each landed with the
 * contents and permissions its characteristics asked for. `initialised`
 * must survive the load with its value; `zeroed` lives in .bss and must
 * arrive as 0; `readonly` proves .rdata was mapped at all. */
static unsigned long long initialised = 0xFEEDFACECAFEBEEFull;
static unsigned long long zeroed;
static const char readonly[] = "rdata";

#define PE_IMAGE_BASE 0x0000000140000000ull
#define PE_IMAGE_SPAN 0x0000000010000000ull   /* generous upper bound */

static int failures = 0;

static void check(const char *what, int ok) {
    report(what, ok);
    if (!ok) failures++;
}

static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

void _start(void) {
    put("pe_test: running from a PE image\n\n");

    /* Where did we actually land? If the loader honoured ImageBase, every
     * address in this image sits just above 0x140000000. */
    uarg here = (uarg)(__UINTPTR_TYPE__)&_start;
    put("  entry  = "); put_hex(here); put("\n");
    put("  data   = ");
    put_hex((unsigned long long)(__UINTPTR_TYPE__)&initialised); put("\n\n");

    check("image loaded at its PE ImageBase",
          here >= PE_IMAGE_BASE && here < PE_IMAGE_BASE + PE_IMAGE_SPAN);

    check(".data arrived with its initialiser",
          initialised == 0xFEEDFACECAFEBEEFull);
    check(".bss arrived zeroed", zeroed == 0);
    check(".rdata is readable", str_eq(readonly, "rdata"));

    /* Writable sections have to be writable , a loader that mapped .data
     * read-only would pass every check above and fail here. */
    initialised = 0x0123456789ABCDEFull;
    check(".data is writable", initialised == 0x0123456789ABCDEFull);
    zeroed = 1;
    check(".bss is writable", zeroed == 1);

    /* The syscall surface works the same from a PE image as from an ELF
     * one: nothing about the ABI changes across the loader. */
    sarg pid = sys1(SYS_GET_PID, 0);
    check("get_pid returns a live pid", pid > 0);

    sarg raw = sys4(SYS_MMAP, 0, 4096, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS);
    void *m = (void *)(__UINTPTR_TYPE__)raw;
    check("mmap from a PE image", raw > 0);
    if (raw > 0) {
        volatile unsigned long long *p = m;
        *p = 0xA5A5A5A5A5A5A5A5ull;
        check("mapped page is usable", *p == 0xA5A5A5A5A5A5A5A5ull);
    }

    if (failures == 0) put("\npe_test: all checks passed\n");
    else               put("\npe_test: FAILURES\n");

    sys1(SYS_EXIT, failures);
    for (;;) { }   /* SYS_EXIT does not return; keep _start noreturn-clean */
}

#include <stddef.h>
#include <sys/mman.h>

// 512 Megabytes!
// If the kernel eagerly allocated this, it would immediately run out of RAM and panic.
#define HUGE_SIZE (512 * 1024 * 1024)

int main() {
    // Ask the kernel for 512MB of memory.
    // With demand paging, this returns instantly using 0 bytes of physical RAM.
    char *huge_mem = mmap(0, HUGE_SIZE, PROT_READ | PROT_WRITE,
                          MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

    if (huge_mem == MAP_FAILED) {
        return 1; // Failed to get virtual address space
    }

    // Now, touch exactly 3 pages out of the 131,072 pages we just mapped.
    // Each of these writes will trigger a page fault, allocating exactly one
    // 4KB frame of physical RAM on the fly.
    huge_mem[0] = 'A';
    huge_mem[4096 * 100] = 'B';   // Touch page 100
    huge_mem[4096 * 5000] = 'C';  // Touch page 5000

    if (huge_mem[0] == 'A' &&
        huge_mem[4096 * 100] == 'B' &&
        huge_mem[4096 * 5000] == 'C') {
        return 0; // Success!
    }

    return 2; // Failure
}

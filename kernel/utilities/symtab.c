/* kernel/utilities/symtab.c — build-time-generated symbol table + resolver. */
#include <stdint.h>
#include <stddef.h>
#include <utilities/symtab.h>

extern char _kernel_start[];
extern char _kernel_end[];

const char *symtab_resolve(uint64_t address, uint64_t *offset_out)
{
    uint64_t base = (uint64_t)(uintptr_t)_kernel_start;
    uint64_t end  = (uint64_t)(uintptr_t)_kernel_end;

    if (address < base || address >= end)
        return NULL;

    if (__ksymtab_count == 0)
        return NULL;

    uint64_t target = address - base;

    /*
     * Binary search for the largest symbol whose offset <= target.
     * The table is sorted by offset at generation time, so the first
     * entry with offset > target marks the upper bound; the preceding
     * entry (if any) is our hit.
     */
    size_t lo = 0, hi = __ksymtab_count, found = (size_t)-1;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (__ksymtab[mid].offset <= target) {
            found = mid;
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    if (found == (size_t)-1)
        return NULL;

    if (offset_out)
        *offset_out = target - __ksymtab[found].offset;

    return __ksymtab[found].name;
}

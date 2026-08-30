/* kernel/utilities/symtab.c , build-time-generated symbol table + resolver. */
#include <stdint.h>
#include <stddef.h>
#include <utilities/symtab.h>

extern char _kernel_start[];
extern char _kernel_end[];

const char *symtab_resolve(u64 address, u64 *offset_out)
{
    u64 base = (u64)(uintptr_t)_kernel_start;
    u64 end  = (u64)(uintptr_t)_kernel_end;

    if (address < base || address >= end)
        return NULL;

    if (__ksymtab_count == 0)
        return NULL;

    u64 target = address - base;

    /*
     * Binary search for the largest symbol whose offset <= target.
     * The table is sorted by offset at generation time, so the first
     * entry with offset > target marks the upper bound; the preceding
     * entry (if any) is our hit.
     */
    usize lo = 0, hi = __ksymtab_count, found = (usize)-1;
    while (lo < hi) {
        usize mid = lo + (hi - lo) / 2;
        if (__ksymtab[mid].offset <= target) {
            found = mid;
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    if (found == (usize)-1)
        return NULL;

    if (offset_out)
        *offset_out = target - __ksymtab[found].offset;

    return __ksymtab[found].name;
}

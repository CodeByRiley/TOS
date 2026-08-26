#ifndef TOS_UTILITIES_SYMTAB_H
#define TOS_UTILITIES_SYMTAB_H

#include "utilities/types.h"
#include <stddef.h>
#include <stdint.h>

/*
 * Minimal kernel symbol table for panic backtraces.
 *
 * Generated at build time by scripts/gensymtab.py from the final kernel
 * ELF.  Entries are sorted by ascending offset and placed in the .ksymtab
 * section so they can be stripped for release builds if desired.
 *
 * Offsets are relative to _kernel_start so the table survives KASLR or
 * any load-address changes without regeneration of individual entries.
 */
struct ksym {
    uint64_t offset;
    const char *name;
};

/*
 * Weak because of the two-pass link: pass one builds kernel_nosyms.elf from
 * these same objects, and the generated table does not exist yet at that
 * point. Weak references resolve to zero there, which leaves __ksymtab_count
 * at 0 and makes symtab_resolve() return NULL before it can dereference the
 * null table. Pass two links the generated definitions in and they win.
 */
extern const struct ksym __ksymtab[] WEAK;
extern const size_t __ksymtab_count WEAK;

/*
 * Resolve a virtual address to the nearest preceding function symbol.
 * Returns the symbol name (NUL-terminated, static storage) or NULL if
 * the address is outside the kernel text or the table is empty.
 *
 * On success, *offset_out receives the byte offset from the symbol's
 * entry point to the queried address.
 */
const char *symtab_resolve(uint64_t address, uint64_t *offset_out);

#endif /* TOS_UTILITIES_SYMTAB_H */

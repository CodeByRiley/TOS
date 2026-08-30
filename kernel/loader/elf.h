/* kernel/loader/elf.h , ELF64 loader.
 *
 * Loads a single statically-linked ELF64 PT_LOAD chain into a target
 * PML4 and returns the entry-point virtual address. Used by process
 * exec/spawn.
 *
 * Implementation: kernel/loader/elf.c.
 */
#ifndef ELF_H
#define ELF_H


/* PACKED and friends. */
#include <utilities/types.h>
#include <stdint.h>

#define PT_LOAD 1
#define PF_X    1
#define PF_W    2
#define PF_R    4

struct PACKED Elf64_Ehdr {
    u8  e_ident[16];
    u16 e_type;
    u16 e_machine;
    u32 e_version;
    u64 e_entry;
    u64 e_phoff;
    u64 e_shoff;
    u32 e_flags;
    u16 e_ehsize;
    u16 e_phentsize;
    u16 e_phnum;
    u16 e_shentsize;
    u16 e_shnum;
    u16 e_shstrndx;
};

struct PACKED Elf64_Phdr {
    u32 p_type;
    u32 p_flags;
    u64 p_offset;
    u64 p_vaddr;
    u64 p_paddr;
    u64 p_filesz;
    u64 p_memsz;
    u64 p_align;
};

/* Load an ELF into `pml4` and return entry-point va, or 0 on failure.
 *
 * CR3-independent: segment bytes go in through the HHDM, so `pml4` need
 * not be the active address space and the caller need not switch to it.
 *
 * On failure the target PML4 may hold partial mappings. The caller owns
 * cleanup , free_user_pml4() reclaims every frame this function mapped. */
u64 elf_load(const char *path, u64 *pml4);

#endif

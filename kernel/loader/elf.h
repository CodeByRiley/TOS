/* kernel/loader/elf.h — ELF64 loader.
 *
 * Loads a single statically-linked ELF64 PT_LOAD chain into a target
 * PML4 and returns the entry-point virtual address. Used by process
 * exec/spawn.
 *
 * Implementation: kernel/loader/elf.c.
 */
#ifndef ELF_H
#define ELF_H

#include <stdint.h>

#define PT_LOAD 1
#define PF_X    1
#define PF_W    2
#define PF_R    4

struct __attribute__((packed)) Elf64_Ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct __attribute__((packed)) Elf64_Phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

/* Load an ELF into `pml4` and return entry-point va, or 0 on failure.
 *
 * CR3-independent: segment bytes go in through the HHDM, so `pml4` need
 * not be the active address space and the caller need not switch to it.
 *
 * On failure the target PML4 may hold partial mappings. The caller owns
 * cleanup — free_user_pml4() reclaims every frame this function mapped. */
uint64_t elf_load(const char *path, uint64_t *pml4);

#endif

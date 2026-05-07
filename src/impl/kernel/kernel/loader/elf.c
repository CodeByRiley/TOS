#include "utilities/string.h"
#include "utilities/log.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "loader/elf.h"
#include "fs/stdio.h"

uint64_t elf_load(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        log_write("elf: fopen failed", KERNEL, LOG_ERROR);
        return 0;
    }

    struct Elf64_Ehdr eh;
    if (fread(&eh, sizeof(eh), 1, fp) != 1) {
        log_write("elf: fread failed for ELF header", KERNEL, LOG_ERROR);
        fclose(fp);
        return 0;
    }
    if (eh.e_ident[0] != 0x7F || eh.e_ident[1] != 'E' ||
        eh.e_ident[2] != 'L' || eh.e_ident[3] != 'F') {
        log_write("elf: bad magic", KERNEL, LOG_ERROR);
        fclose(fp);
        return 0;
    }
    if (eh.e_machine != 62) {
        log_write("elf: not x86_64", KERNEL, LOG_ERROR);
        fclose(fp);
        return 0;
    }

    log_write_hex("elf: entry =", eh.e_entry, KERNEL, LOG_INFO);
    log_write_hex("elf: phnum =", eh.e_phnum, KERNEL, LOG_INFO);

    for (int i = 0; i < eh.e_phnum; i++) {
        struct Elf64_Phdr ph;
        fseek(fp, eh.e_phoff + i * eh.e_phentsize, SEEK_SET);
        if (fread(&ph, sizeof(ph), 1, fp) != 1) {
            log_write("elf: fread failed for program header", KERNEL, LOG_ERROR);
            fclose(fp);
            return 0;
        }
        if (ph.p_type != PT_LOAD) continue;

        uint64_t va_start = ph.p_vaddr & ~0xFFFULL;
        uint64_t va_end   = (ph.p_vaddr + ph.p_memsz + 0xFFF) & ~0xFFFULL;

        uint64_t flags = VMM_PRESENT | VMM_USER;
        if (ph.p_flags & PF_W) flags |= VMM_WRITE;

        for (uint64_t va = va_start; va < va_end; va += 4096) {
            if (!vmm_translate(va)) {
                uint64_t phys = pmm_alloc_frame();
                if (!phys) {
                    log_write("elf: failed to allocate physical frame", KERNEL, LOG_ERROR);
                    fclose(fp);
                    return 0;
                }
                vmm_map(va, phys, flags);
            }
        }
        // zero entire region so reused pages don't keep stale bytes
        memset((void*)va_start, 0, va_end - va_start);

        // copy file bytes into user virt (kernel can write through user mapping)
        fseek(fp, ph.p_offset, SEEK_SET);
        size_t got = fread((void*)ph.p_vaddr, 1, ph.p_filesz, fp);
        if (got != ph.p_filesz) {
            log_write("elf: could not read full segment data", KERNEL, LOG_ERROR);
            fclose(fp);
            return 0;
        }

        log_write_hex("elf: loaded segment va =", ph.p_vaddr,  KERNEL, LOG_INFO);
        log_write_hex("elf: filesz           =", ph.p_filesz, KERNEL, LOG_INFO);
        log_write_hex("elf: memsz            =", ph.p_memsz,  KERNEL, LOG_INFO);
    }

    fclose(fp);
    return eh.e_entry;
}

// uint64_t elf_load(const char *path) {
//     FILE *fp = fopen(path, "rb");
//     if (!fp) {
//         log_write("elf: fopen failed", KERNEL, LOG_ERROR);
//         return 0;
//     }

//     struct Elf64_Ehdr eh;
//     if (fread(&eh, sizeof(eh), 1, fp) != 1) {
//         fclose(fp);
//         return 0;
//     }
//     if (eh.e_ident[0] != 0x7F || eh.e_ident[1] != 'E' ||
//         eh.e_ident[2] != 'L'  || eh.e_ident[3] != 'F') {
//         log_write("elf: bad magic", KERNEL, LOG_ERROR);
//         fclose(fp);
//         return 0;
//     }
//     if (eh.e_machine != 62) {
//         log_write("elf: not x86_64", KERNEL, LOG_ERROR);
//         fclose(fp);
//         return 0;
//     }

//     log_write_hex("elf: entry =", eh.e_entry, KERNEL, LOG_INFO);
//     log_write_hex("elf: phnum =", eh.e_phnum, KERNEL, LOG_INFO);

//     for (int i = 0; i < eh.e_phnum; i++) {
//         struct Elf64_Phdr ph;
//         fseek(fp, eh.e_phoff + i * eh.e_phentsize, SEEK_SET);
//         if (fread(&ph, sizeof(ph), 1, fp) != 1) { fclose(fp); return 0; }
//         if (ph.p_type != PT_LOAD) continue;

//         uint64_t va_start = ph.p_vaddr & ~0xFFFULL;
//         uint64_t va_end   = (ph.p_vaddr + ph.p_memsz + 0xFFF) & ~0xFFFULL;

//         uint64_t flags = VMM_PRESENT | VMM_USER;
//         if (ph.p_flags & PF_W) flags |= VMM_WRITE;

//         for (uint64_t va = va_start; va < va_end; va += 4096) {
//             if (vmm_translate(va)) continue;            // already mapped
//             uint64_t phys = pmm_alloc_frame();
//             if (!phys) { fclose(fp); return 0; }
//             memset((void*)phys, 0, 4096);
//             vmm_map(va, phys, flags);
//         }

//         // copy file bytes into user virt (kernel can write through user mapping)
//         fseek(fp, ph.p_offset, SEEK_SET);
//         size_t got = fread((void*)ph.p_vaddr, 1, ph.p_filesz, fp);
//         log_write_hex("elf: loaded segment va =", ph.p_vaddr,  KERNEL, LOG_INFO);
//         log_write_hex("elf: filesz           =", ph.p_filesz, KERNEL, LOG_INFO);
//         log_write_hex("elf: memsz            =", ph.p_memsz,  KERNEL, LOG_INFO);
//         (void)got;
//         // BSS region (memsz > filesz) already zeroed by memset above
//     }

//     fclose(fp);
//     return eh.e_entry;
// }

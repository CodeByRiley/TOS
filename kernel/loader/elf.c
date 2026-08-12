/* kernel/loader/elf.c — ELF64 loader.
 *
 * The world's smallest ELF64 loader: opens the file via the FAT-backed
 * FILE* layer, validates magic + machine type, walks the program header
 * table, and copies every PT_LOAD segment into the target PML4 with the
 * requested permissions.
 *
 * Segment bytes are written through the HHDM rather than through the user
 * vaddr, so the loader does not care which PML4 is in CR3. Do not "simplify"
 * this back into a straight memcpy to p_vaddr: that only works while the
 * caller has switched CR3 to the target, and it silently corrupts the
 * caller's address space when it hasn't.
 */
#include "utilities/string.h"
#include "utilities/log.h"
#include "memory/pmm.h"
#include "memory/hhdm.h"
#include "memory/vmm.h"
#include "loader/elf.h"
#include "loader/process.h"
#include "fs/stdio.h"

/* USER_IMAGE_MAX (loader/process.h) bounds the image region: it sits below
 * the mmap arena so a segment can never land on mmap or shmem, and it
 * doubles as the bound that keeps p_vaddr + p_memsz from wrapping. */

uint64_t elf_load(const char *path, uint64_t *pml4) {
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

    for (int i = 0; i < eh.e_phnum; i++) {
        struct Elf64_Phdr ph;
        fseek(fp, eh.e_phoff + i * eh.e_phentsize, SEEK_SET);
        if (fread(&ph, sizeof(ph), 1, fp) != 1) {
            log_write("elf: fread failed for program header", KERNEL, LOG_ERROR);
            fclose(fp);
            return 0;
        }
        if (ph.p_type != PT_LOAD) continue;

        /* Pages are mapped for p_memsz but bytes are read for p_filesz.
         * A filesz larger than memsz writes past the end of the mapping. */
        if (ph.p_filesz > ph.p_memsz) {
            log_write("elf: filesz exceeds memsz", KERNEL, LOG_ERROR);
            fclose(fp);
            return 0;
        }

        /* p_vaddr comes straight off disk and is fully attacker-controlled.
         * Unchecked, a kernel-half vaddr walks into PML4[256..511] — which
         * process_pml4_create shares by physical address with kernel_pml4 —
         * and walk_or_create ORs VMM_USER into those shared entries on the
         * way down. That hands ring 3 a mapping in the kernel half of every
         * address space. Bound memsz first so the second test cannot wrap. */
        if (ph.p_memsz > USER_IMAGE_MAX ||
            ph.p_vaddr > USER_IMAGE_MAX - ph.p_memsz) {
            log_write("elf: segment vaddr out of range", KERNEL, LOG_ERROR);
            fclose(fp);
            return 0;
        }

        uint64_t va_start = ph.p_vaddr & ~0xFFFULL;
        uint64_t va_end   = (ph.p_vaddr + ph.p_memsz + 0xFFF) & ~0xFFFULL;

        /* NX on anything the ELF does not mark executable. Depends on
         * EFER.NXE, which enable_paging sets on the BSP and the AP
         * trampoline sets on every other core; check_long_mode has
         * already proven CPUID reports NX support.
         *
         * Safe despite the loader not merging flags on pages two segments
         * share: user.ld page-aligns .data, so the R E and RW segments
         * never land on the same page. If that ever changes, the first
         * segment to map a shared page wins and an NX-first ordering
         * would make .text unexecutable. */
        uint64_t flags = VMM_PRESENT | VMM_USER;
        if (ph.p_flags & PF_W) flags |= VMM_WRITE;
        if (!(ph.p_flags & PF_X)) flags |= VMM_NX;

        /* Map missing pages and zero only the freshly-allocated frames.
         *
         * Previous version did one big `memset(va_start, 0, va_end - va_start)`
         * AFTER the loop, which had the side effect of erasing every page that
         * a prior PT_LOAD segment happened to share with this one — typically
         * the page straddling a segment boundary. fread then refilled only the
         * second segment's bytes, leaving a confetti'd hole where the first
         * segment used to be. ELFs are aligned enough that this almost never
         * actually fires in practice, which is exactly the kind of bug that
         * shows up months later wearing a costume. */
        for (uint64_t va = va_start; va < va_end; va += 4096) {
            if (!vmm_translate_in(pml4, va)) {
                uint64_t phys = pmm_alloc_frame();
                if (!phys) {
                    log_write("elf: failed to allocate physical frame", KERNEL, LOG_ERROR);
                    fclose(fp);
                    return 0;
                }
                memset(phys_to_virt(phys), 0, 4096);
                /* Free on failure: an allocated-but-unmapped frame is
                 * invisible to free_user_pml4's page-table walk, so the
                 * caller's cleanup would never reclaim it. */
                if (vmm_map_in(pml4, va, phys, flags) != 0) {
                    pmm_free_frame(phys);
                    log_write("elf: map failed", KERNEL, LOG_ERROR);
                    fclose(fp);
                    return 0;
                }
            }
        }

        /* Copy file bytes a page at a time through the HHDM. Consecutive
         * user vaddrs are not consecutive physical frames, so this cannot
         * be one flat read. BSS (memsz > filesz) keeps the zeros above. */
        fseek(fp, ph.p_offset, SEEK_SET);
        for (uint64_t done = 0; done < ph.p_filesz; ) {
            uint64_t va    = ph.p_vaddr + done;
            /* Page-aligned vaddr in, so the return is a clean frame base
             * and the offset below is not double-counted. */
            uint64_t phys  = vmm_translate_in(pml4, va & ~0xFFFULL);
            if (!phys) {
                log_write("elf: segment page not mapped", KERNEL, LOG_ERROR);
                fclose(fp);
                return 0;
            }
            size_t   chunk = 4096 - (va & 0xFFF);
            if (chunk > ph.p_filesz - done) chunk = ph.p_filesz - done;
            if (fread((uint8_t*)phys_to_virt(phys) + (va & 0xFFF), 1, chunk, fp) != chunk) {
                log_write("elf: could not read full segment data", KERNEL, LOG_ERROR);
                fclose(fp);
                return 0;
            }
            done += chunk;
        }

    }

    fclose(fp);
    return eh.e_entry;
}

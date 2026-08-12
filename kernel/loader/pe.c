#include "utilities/string.h"
#include "utilities/log.h"
#include "memory/pmm.h"
#include "memory/hhdm.h"
#include "memory/vmm.h"
#include "loader/pe.h"
#include "loader/process.h"
#include "fs/stdio.h"

#define IMAGE_SCN_MEM_EXECUTE 0x20000000
#define IMAGE_SCN_MEM_READ    0x40000000
#define IMAGE_SCN_MEM_WRITE   0x80000000

uint64_t pe_load(const char *path, uint64_t *pml4) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        log_write("pe: fopen failed", KERNEL, LOG_ERROR);
        return 0;
    }

    struct IMAGE_DOS_HEADER dos;
    if (fread(&dos, sizeof(dos), 1, fp) != 1) {
        log_write("pe: fread failed for DOS header", KERNEL, LOG_ERROR);
        fclose(fp);
        return 0;
    }
    if (dos.e_magic != 0x5A4D) { // "MZ"
        log_write_hex("pe: bad DOS magic, expected 0x5A4D, got ", dos.e_magic, KERNEL, LOG_ERROR);
        fclose(fp);
        return 0;
    }

    struct IMAGE_NT_HEADERS64 nt;
    fseek(fp, dos.e_lfanew, SEEK_SET);
    if (fread(&nt, sizeof(nt), 1, fp) != 1) {
        log_write("pe: fread failed for NT headers", KERNEL, LOG_ERROR);
        fclose(fp);
        return 0;
    }
    if (nt.signature != 0x00004550) { // "PE\0\0"
        log_write_hex("pe: bad PE signature, expected 0x00004550, got ", nt.signature, KERNEL, LOG_ERROR);
        fclose(fp);
        return 0;
    }
    if (nt.fileHeader.machine != 0x8664) {
        log_write("pe: not x86_64", KERNEL, LOG_ERROR);
        fclose(fp);
        return 0;
    }

    uint64_t image_base = nt.optionalHeader.imageBase;
    uint32_t size_of_image = nt.optionalHeader.sizeOfImage;
    uint32_t entry_rva = nt.optionalHeader.addressOfEntryPoint;

    /* In ELF, PT_LOAD segments are typically under 0x70000000.
     * In 64-bit PE, ImageBase is usually 0x140000000 (5 GiB).
     * We must allow it to land in the MAP_FIXED region.
     * We still bound check it so it doesn't run into kernel space (PML4[256..511]). */
    uint64_t kernel_half_limit = 0xFFFF800000000000ULL; // or whatever your upper bound is

    if (size_of_image > kernel_half_limit ||
        image_base > kernel_half_limit - size_of_image) {
        log_write("pe: image base or size out of range", KERNEL, LOG_ERROR);
        fclose(fp);
        return 0;
    }

    /* In ELF, you loop PT_LOAD segments. In PE, you loop Section Headers.
     * They sit immediately after the OptionalHeader. */
    uint32_t num_sections = nt.fileHeader.numberOfSections;
    uint32_t size_of_optional = nt.fileHeader.sizeOfOptionalHeader;

    /* Skip file pointer past the NT headers to the first section header */
    long sections_offset = dos.e_lfanew + sizeof(nt.signature) + sizeof(struct IMAGE_FILE_HEADER) + size_of_optional;

    for (int i = 0; i < num_sections; i++) {
        struct IMAGE_SECTION_HEADER sh;
        fseek(fp, sections_offset + i * sizeof(sh), SEEK_SET);
        if (fread(&sh, sizeof(sh), 1, fp) != 1) {
            log_write("pe: fread failed for section header", KERNEL, LOG_ERROR);
            fclose(fp);
            return 0;
        }

        /* Sections with no raw data (like .bss) can be skipped if VirtualSize is 0 */
        if (sh.virtualSize == 0 && sh.sizeOfRawData == 0) continue;

        /* PE VirtualSize is like p_memsz. SizeOfRawData is like p_filesz. */
        uint32_t memsz = sh.virtualSize;
        uint32_t filesz = sh.sizeOfRawData;
        if (filesz > memsz) memsz = filesz; // Sometimes SizeOfRawData is larger

        /* VirtualAddress is an RVA. Actual VA is ImageBase + VirtualAddress */
        uint64_t va_base = image_base + sh.virtualAddress;
        /* Allow PE sections to map up to the kernel half boundary */
        uint64_t user_limit = 0x0000800000000000ULL;
        if (memsz > user_limit || va_base > user_limit - memsz) {
            log_write("pe: section vaddr out of range", KERNEL, LOG_ERROR);
            fclose(fp);
            return 0;
        }

        uint64_t va_start = va_base & ~0xFFFULL;
        uint64_t va_end   = (va_base + memsz + 0xFFF) & ~0xFFFULL;

        /* Map permissions based on Section Characteristics */
        uint64_t flags = VMM_PRESENT | VMM_USER;
        if (sh.characteristics & IMAGE_SCN_MEM_WRITE) flags |= VMM_WRITE;
        if (!(sh.characteristics & IMAGE_SCN_MEM_EXECUTE)) flags |= VMM_NX;

        /* Map missing pages and zero them, just like ELF */
        for (uint64_t va = va_start; va < va_end; va += 4096) {
            if (!vmm_translate_in(pml4, va)) {
                uint64_t phys = pmm_alloc_frame();
                if (!phys) {
                    log_write("pe: failed to allocate physical frame", KERNEL, LOG_ERROR);
                    fclose(fp);
                    return 0;
                }
                memset(phys_to_virt(phys), 0, 4096);
                if (vmm_map_in(pml4, va, phys, flags) != 0) {
                    pmm_free_frame(phys);
                    log_write("pe: map failed", KERNEL, LOG_ERROR);
                    fclose(fp);
                    return 0;
                }
            }
        }

        /* Copy file bytes a page at a time through the HHDM */
        if (filesz > 0) {
            fseek(fp, sh.pointerToRawData, SEEK_SET);
            for (uint64_t done = 0; done < filesz; ) {
                uint64_t va    = va_base + done;
                uint64_t phys  = vmm_translate_in(pml4, va & ~0xFFFULL);
                if (!phys) {
                    log_write("pe: section page not mapped", KERNEL, LOG_ERROR);
                    fclose(fp);
                    return 0;
                }
                size_t   chunk = 4096 - (va & 0xFFF);
                if (chunk > filesz - done) chunk = filesz - done;
                if (fread((uint8_t*)phys_to_virt(phys) + (va & 0xFFF), 1, chunk, fp) != chunk) {
                    log_write("pe: could not read full section data", KERNEL, LOG_ERROR);
                    fclose(fp);
                    return 0;
                }
                done += chunk;
            }
        }

    }

    fclose(fp);

    /* Return absolute VA. In ELF e_entry is absolute; in PE AddressOfEntryPoint is an RVA */
    return image_base + entry_rva;
}

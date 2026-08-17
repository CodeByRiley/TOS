/**
 * @file pe.c
 * @brief PE (Portable Executable) image loader implementation.
 *
 * Provides the functionality to parse, map, and load a 64-bit PE binary
 * into memory, preparing it for execution. Conceptually mirrors an ELF
 * loader but adapts to the PE section-based layout.
 */

#include <utilities/string.h>
#include <utilities/log.h>
#include <memory/pmm.h>
#include <memory/hhdm.h>
#include <memory/vmm.h>
#include <loader/pe.h>
#include <loader/process.h>
#include <fs/stdio.h>
#include <sched/sched.h>

/// Section characteristic flag: Section can be executed as code.
#define IMAGE_SCN_MEM_EXECUTE 0x20000000
/// Section characteristic flag: Section can be read.
#define IMAGE_SCN_MEM_READ    0x40000000
/// Section characteristic flag: Section can be written to.
#define IMAGE_SCN_MEM_WRITE   0x80000000

/**
 * @brief Load a PE image from disk and prepare it for execution.
 *
 * Reads the PE file located at @p path, parses its DOS/NT headers, and
 * iterates through its section headers to map the image into memory.
 * It handles memory allocation, page-level permissions (NX, Write),
 * and copies file data into the correct virtual addresses.
 *
 * @param path  Filesystem path of the PE binary to load.
 * @param pml4  Pointer to a 64-bit value containing the physical address
 *              of the target process's PML4 (top-level page table).
 * @return The virtual address of the image's entry point, or 0 on failure.
 */
uint64_t pe_load(const char *path, uint64_t *pml4) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        log_write("pe: fopen failed", KERNEL, LOG_ERROR);
        return 0;
    }

    // Parse DOS Header
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

    // Parse NT Headers
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
    uint64_t kernel_half_limit = 0xFFFF800000000000ULL; // Upper bound for kernel space

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

    // Calculate file offset to the beginning of the section header table
    long sections_offset = dos.e_lfanew + sizeof(nt.signature) + sizeof(struct IMAGE_FILE_HEADER) + size_of_optional;

    // Iterate and Map Sections
    for (int i = 0; i < num_sections; i++) {
        struct IMAGE_SECTION_HEADER sh;
        fseek(fp, sections_offset + i * sizeof(sh), SEEK_SET);
        if (fread(&sh, sizeof(sh), 1, fp) != 1) {
            log_write("pe: fread failed for section header", KERNEL, LOG_ERROR);
            fclose(fp);
            return 0;
        }

        // Skip empty sections (e.g., .bss variants with no data)
        if (sh.virtualSize == 0 && sh.sizeOfRawData == 0) continue;

        /* PE VirtualSize is like p_memsz. SizeOfRawData is like p_filesz. */
        uint32_t memsz = sh.virtualSize;
        uint32_t filesz = sh.sizeOfRawData;
        if (filesz > memsz) memsz = filesz; // Sometimes SizeOfRawData is larger due to file alignment

        /* VirtualAddress is an RVA. Actual VA is ImageBase + VirtualAddress */
        uint64_t va_base = image_base + sh.virtualAddress;

        // Bound check the section's virtual address range against user space limits
        uint64_t user_limit = 0x0000800000000000ULL;
        if (memsz > user_limit || va_base > user_limit - memsz) {
            log_write("pe: section vaddr out of range", KERNEL, LOG_ERROR);
            fclose(fp);
            return 0;
        }

        // Page-align the memory region to be mapped
        uint64_t va_start = va_base & ~0xFFFULL;
        uint64_t va_end   = (va_base + memsz + 0xFFF) & ~0xFFFULL;

        // Determine VMM mapping flags based on PE section characteristics
        uint64_t flags = VMM_PRESENT | VMM_USER;
        if (sh.characteristics & IMAGE_SCN_MEM_WRITE) flags |= VMM_WRITE;
        if (!(sh.characteristics & IMAGE_SCN_MEM_EXECUTE)) flags |= VMM_NX;

        // Allocate and Map Pages
        // Map missing pages and zero them, just like ELF PT_LOAD bss handling
        int mapped_since_yield = 0;
        for (uint64_t va = va_start; va < va_end; va += 4096) {
            if (!vmm_translate_in(pml4, va)) {
                uint64_t phys = pmm_alloc_frame();
                if (!phys) {
                    log_write("pe: failed to allocate physical frame", KERNEL, LOG_ERROR);
                    fclose(fp);
                    return 0;
                }
                memset(phys_to_virt(phys), 0, 4096); // Zero out new pages
                if (vmm_map_in(pml4, va, phys, flags) != 0) {
                    pmm_free_frame(phys);
                    log_write("pe: map failed", KERNEL, LOG_ERROR);
                    fclose(fp);
                    return 0;
                }
            }
            // Yield periodically to prevent locking up the scheduler during large mappings
            if (++mapped_since_yield == 32) {
                mapped_since_yield = 0;
                task_yield();
            }
        }

        // Copy File Data into Mapped Pages
        if (filesz > 0) {
            fseek(fp, sh.pointerToRawData, SEEK_SET);
            int copied_since_yield = 0;
            for (uint64_t done = 0; done < filesz; ) {
                uint64_t va    = va_base + done;
                uint64_t phys  = vmm_translate_in(pml4, va & ~0xFFFULL);
                if (!phys) {
                    log_write("pe: section page not mapped", KERNEL, LOG_ERROR);
                    fclose(fp);
                    return 0;
                }

                // Calculate how much we can copy into the current page
                size_t   chunk = 4096 - (va & 0xFFF);
                if (chunk > filesz - done) chunk = filesz - done;

                // Write data through the HHDM (Higher Half Direct Map)
                if (fread((uint8_t*)phys_to_virt(phys) + (va & 0xFFF), 1, chunk, fp) != chunk) {
                    log_write("pe: could not read full section data", KERNEL, LOG_ERROR);
                    fclose(fp);
                    return 0;
                }
                done += chunk;

                if (++copied_since_yield == 32) {
                    copied_since_yield = 0;
                    task_yield();
                }
            }
        }
    }

    fclose(fp);

    // Return Entry Point
    // In ELF e_entry is absolute; in PE AddressOfEntryPoint is an RVA.
    // Return absolute VA by adding the ImageBase.
    return image_base + entry_rva;
}

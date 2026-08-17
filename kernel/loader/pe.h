#ifndef PE_H
#define PE_H

#include <stdint.h>

/**
 * @brief Legacy DOS stub header that every PE image begins with.
 *
 * Although modern Windows never executes the DOS stub, this header is
 * preserved for backwards compatibility. Its most important field is
 * @ref e_lfanew, which gives the file offset of the real PE header
 * (@ref IMAGE_NT_HEADERS64). This is conceptually similar to the very
 * beginning of an ELF file, but with historical DOS junk in front of
 * the actual format data.
 *
 * The structure is packed to match its on-disk layout exactly.
 */
struct __attribute__((packed)) IMAGE_DOS_HEADER {
    uint16_t e_magic;      ///< Magic number: 0x5A4D (ASCII "MZ").
    uint16_t e_cblp;       ///< Bytes on last page of file.
    uint16_t e_cp;         ///< Pages in file.
    uint16_t e_crlc;       ///< Relocations.
    uint16_t e_cparhdr;    ///< Size of header in paragraphs.
    uint16_t e_minalloc;   ///< Minimum extra paragraphs needed.
    uint16_t e_maxalloc;   ///< Maximum extra paragraphs needed.
    uint16_t e_ss;         ///< Initial (relative) SS value.
    uint16_t e_sp;         ///< Initial SP value.
    uint16_t e_csum;       ///< Checksum.
    uint16_t e_ip;         ///< Initial IP value.
    uint16_t e_cs;         ///< Initial (relative) CS value.
    uint16_t e_lfarlc;     ///< File address of relocation table.
    uint16_t e_ovno;       ///< Overlay number.
    uint16_t e_res[4];     ///< Reserved words.
    uint16_t e_oemid;      ///< OEM identifier (for e_oeminfo).
    uint16_t e_oeminfo;    ///< OEM information; e_oemid specific.
    uint16_t e_res2[10];   ///< Reserved words.
    uint32_t e_lfanew;     ///< File offset to the PE header (IMAGE_NT_HEADERS64).
};

/**
 * @brief COFF-style file header that describes the basic properties of
 *        the image (machine type, number of sections, etc.).
 *
 * This header immediately follows the 4-byte PE signature inside
 * @ref IMAGE_NT_HEADERS64. It is roughly analogous to the metadata
 * portion of an ELF `Elf64_Ehdr`.
 */
struct __attribute__((packed)) IMAGE_FILE_HEADER {
    uint16_t machine;              ///< Target machine type (e.g. 0x8664 for x64).
    uint16_t numberOfSections;     ///< Number of entries in the section table (Phdr-equivalents).
    uint32_t timeDateStamp;        ///< Time & date the image was created (Unix-style stamp).
    uint32_t pointerToSymbolTable; ///< File offset of COFF symbol table (usually 0 for modern images).
    uint32_t numberOfSymbols;      ///< Number of entries in the symbol table.
    uint16_t sizeOfOptionalHeader; ///< Size in bytes of the following IMAGE_OPTIONAL_HEADER64.
    uint16_t characteristics;      ///< Image flags (e.g. executable, DLL, large-address-aware).
};

/**
 * @brief A single entry in the data directory array.
 *
 * Each entry locates an important table (imports, exports, relocations,
 * resources, etc.) within the image by giving its Relative Virtual
 * Address (RVA) and size in bytes.
 */
struct __attribute__((packed)) IMAGE_DATA_DIRECTORY {
    uint32_t virtualAddress; ///< RVA of the table/data.
    uint32_t size;           ///< Size of the table/data, in bytes.
};

/**
 * @brief The "optional" (but mandatory for executables) PE32+ header.
 *
 * Despite its name, this header is required for any image meant to be
 * loaded by the OS loader. It carries the information needed to map the
 * file into memory: entry point, preferred image base, alignment
 * requirements, stack/heap sizes, subsystem, and the data directory.
 *
 * Total size: 240 bytes.
 */
struct __attribute__((packed)) IMAGE_OPTIONAL_HEADER64 {
    uint16_t magic;                       ///< Magic: 0x20B identifies PE32+ (64-bit).
    uint8_t  majorLinkerVersion;          ///< Major version of the linker.
    uint8_t  minorLinkerVersion;          ///< Minor version of the linker.
    uint32_t sizeOfCode;                  ///< Size of the .text (code) section, summed across all code sections.
    uint32_t sizeOfInitializedData;       ///< Size of all initialized-data sections.
    uint32_t sizeOfUninitializedData;     ///< Size of all uninitialized-data (BSS-like) sections.
    uint32_t addressOfEntryPoint;         ///< RVA of the entry point (analogous to ELF e_entry).
    uint32_t baseOfCode;                  ///< RVA of the first code section.
    uint64_t imageBase;                   ///< Preferred load address for the image in virtual memory.
    uint32_t sectionAlignment;            ///< Alignment of sections in memory (typically 0x1000).
    uint32_t fileAlignment;               ///< Alignment of sections on disk (typically 0x200).
    uint16_t majorOperatingSystemVersion; ///< Minimum required OS version (major).
    uint16_t minorOperatingSystemVersion; ///< Minimum required OS version (minor).
    uint16_t majorImageVersion;           ///< Image version (major), user-defined.
    uint16_t minorImageVersion;           ///< Image version (minor), user-defined.
    uint16_t majorSubsystemVersion;       ///< Subsystem version required (major).
    uint16_t minorSubsystemVersion;       ///< Subsystem version required (minor).
    uint32_t win32VersionValue;           ///< Reserved, must be 0.
    uint32_t sizeOfImage;                 ///< Total virtual size of the image (RAM to allocate).
    uint32_t sizeOfHeaders;               ///< Size on disk of DOS + NT + section headers (rounded up to fileAlignment).
    uint32_t checkSum;                    ///< Image checksum (used by the loader for validation of drivers/system DLLs).
    uint16_t subsystem;                   ///< Target subsystem (e.g. Windows GUI = 2, Windows CUI = 3).
    uint16_t dllCharacteristics;          ///< DLL characteristics flags (e.g. ASLR, DEP/NX, CFG).
    uint64_t sizeOfStackReserve;          ///< Stack size to reserve for the main thread.
    uint64_t sizeOfStackCommit;           ///< Stack size to commit for the main thread.
    uint64_t sizeOfHeapReserve;           ///< Default heap size to reserve.
    uint64_t sizeOfHeapCommit;            ///< Default heap size to commit.
    uint32_t loaderFlags;                 ///< Reserved, must be 0.
    uint32_t numberOfRvaAndSizes;         ///< Number of valid entries in dataDirectory (usually 16).
    struct IMAGE_DATA_DIRECTORY dataDirectory[16]; ///< Directory of imports, exports, relocations, resources, etc.
};

/**
 * @brief The main PE header, located at the file offset given by
 *        @ref IMAGE_DOS_HEADER::e_lfanew.
 *
 * Layout on disk:
 *   - 4-byte signature ("PE\0\0")
 *   - IMAGE_FILE_HEADER
 *   - IMAGE_OPTIONAL_HEADER64
 *
 * This is the conceptual equivalent of `Elf64_Ehdr` for the PE format.
 */
struct __attribute__((packed)) IMAGE_NT_HEADERS64 {
    uint32_t signature;                         ///< PE signature: 0x00004550 (ASCII "PE\0\0").
    struct IMAGE_FILE_HEADER fileHeader;        ///< COFF file header.
    struct IMAGE_OPTIONAL_HEADER64 optionalHeader; ///< PE32+ optional header.
};

/**
 * @brief Describes a single section of the image (analogous to `Elf64_Phdr`).
 *
 * The section table is an array of these structures, located immediately
 * after the optional header. Each entry maps a chunk of the file
 * (pointerToRawData, sizeOfRawData) to a chunk of the in-memory image
 * (virtualAddress, virtualSize).
 *
 * Total size: 40 bytes.
 */
struct __attribute__((packed)) IMAGE_SECTION_HEADER {
    uint8_t  name[8];                  ///< Null-padded section name (e.g. ".text", ".data").
    uint32_t virtualSize;              ///< Size of the section in memory (analogous to p_memsz).
    uint32_t virtualAddress;           ///< RVA of the section in memory (add imageBase for a full VA).
    uint32_t sizeOfRawData;            ///< Size of the section on disk (analogous to p_filesz).
    uint32_t pointerToRawData;         ///< File offset of the section's raw data (analogous to p_offset).
    uint32_t pointerToRelocations;     ///< File offset of relocations for this section (object files only).
    uint32_t pointerToLinenumbers;     ///< File offset of line-number entries (deprecated, COFF debug info).
    uint16_t numberOfRelocations;      ///< Number of relocation entries for this section.
    uint16_t numberOfLinenumbers;      ///< Number of line-number entries for this section.
    uint32_t characteristics;          ///< Section flags (analogous to p_flags): executable, readable, writable, etc.
};

/**
 * @brief Load a PE image from disk and prepare it for execution.
 *
 * Reads the PE file located at @p path, parses its headers, allocates
 * memory for the image, maps each section to its proper virtual address,
 * applies base relocations, resolves imports, and returns the entry
 * point address.
 *
 * @param path  Filesystem path of the PE binary to load.
 * @param pml4   Pointer to a 64-bit value that will receive the address
 *               of the top-level (PML4) page table constructed for the
 *               new address space. May be `NULL` if the caller does not
 *               need this information.
 *
 * @return The virtual address of the image's entry point, or `0` on
 *         failure.
 */
uint64_t pe_load(const char* path, uint64_t *pml4);

#endif // PE_H

#ifndef PE_H
#define PE_H


/* PACKED and friends. */
#include <utilities/types.h>
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
struct PACKED IMAGE_DOS_HEADER {
    u16 e_magic;      ///< Magic number: 0x5A4D (ASCII "MZ").
    u16 e_cblp;       ///< Bytes on last page of file.
    u16 e_cp;         ///< Pages in file.
    u16 e_crlc;       ///< Relocations.
    u16 e_cparhdr;    ///< Size of header in paragraphs.
    u16 e_minalloc;   ///< Minimum extra paragraphs needed.
    u16 e_maxalloc;   ///< Maximum extra paragraphs needed.
    u16 e_ss;         ///< Initial (relative) SS value.
    u16 e_sp;         ///< Initial SP value.
    u16 e_csum;       ///< Checksum.
    u16 e_ip;         ///< Initial IP value.
    u16 e_cs;         ///< Initial (relative) CS value.
    u16 e_lfarlc;     ///< File address of relocation table.
    u16 e_ovno;       ///< Overlay number.
    u16 e_res[4];     ///< Reserved words.
    u16 e_oemid;      ///< OEM identifier (for e_oeminfo).
    u16 e_oeminfo;    ///< OEM information; e_oemid specific.
    u16 e_res2[10];   ///< Reserved words.
    u32 e_lfanew;     ///< File offset to the PE header (IMAGE_NT_HEADERS64).
};

/**
 * @brief COFF-style file header that describes the basic properties of
 *        the image (machine type, number of sections, etc.).
 *
 * This header immediately follows the 4-byte PE signature inside
 * @ref IMAGE_NT_HEADERS64. It is roughly analogous to the metadata
 * portion of an ELF `Elf64_Ehdr`.
 */
struct PACKED IMAGE_FILE_HEADER {
    u16 machine;              ///< Target machine type (e.g. 0x8664 for x64).
    u16 numberOfSections;     ///< Number of entries in the section table (Phdr-equivalents).
    u32 timeDateStamp;        ///< Time & date the image was created (Unix-style stamp).
    u32 pointerToSymbolTable; ///< File offset of COFF symbol table (usually 0 for modern images).
    u32 numberOfSymbols;      ///< Number of entries in the symbol table.
    u16 sizeOfOptionalHeader; ///< Size in bytes of the following IMAGE_OPTIONAL_HEADER64.
    u16 characteristics;      ///< Image flags (e.g. executable, DLL, large-address-aware).
};

/**
 * @brief A single entry in the data directory array.
 *
 * Each entry locates an important table (imports, exports, relocations,
 * resources, etc.) within the image by giving its Relative Virtual
 * Address (RVA) and size in bytes.
 */
struct PACKED IMAGE_DATA_DIRECTORY {
    u32 virtualAddress; ///< RVA of the table/data.
    u32 size;           ///< Size of the table/data, in bytes.
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
struct PACKED IMAGE_OPTIONAL_HEADER64 {
    u16 magic;                       ///< Magic: 0x20B identifies PE32+ (64-bit).
    u8  majorLinkerVersion;          ///< Major version of the linker.
    u8  minorLinkerVersion;          ///< Minor version of the linker.
    u32 sizeOfCode;                  ///< Size of the .text (code) section, summed across all code sections.
    u32 sizeOfInitializedData;       ///< Size of all initialized-data sections.
    u32 sizeOfUninitializedData;     ///< Size of all uninitialized-data (BSS-like) sections.
    u32 addressOfEntryPoint;         ///< RVA of the entry point (analogous to ELF e_entry).
    u32 baseOfCode;                  ///< RVA of the first code section.
    u64 imageBase;                   ///< Preferred load address for the image in virtual memory.
    u32 sectionAlignment;            ///< Alignment of sections in memory (typically 0x1000).
    u32 fileAlignment;               ///< Alignment of sections on disk (typically 0x200).
    u16 majorOperatingSystemVersion; ///< Minimum required OS version (major).
    u16 minorOperatingSystemVersion; ///< Minimum required OS version (minor).
    u16 majorImageVersion;           ///< Image version (major), user-defined.
    u16 minorImageVersion;           ///< Image version (minor), user-defined.
    u16 majorSubsystemVersion;       ///< Subsystem version required (major).
    u16 minorSubsystemVersion;       ///< Subsystem version required (minor).
    u32 win32VersionValue;           ///< Reserved, must be 0.
    u32 sizeOfImage;                 ///< Total virtual size of the image (RAM to allocate).
    u32 sizeOfHeaders;               ///< Size on disk of DOS + NT + section headers (rounded up to fileAlignment).
    u32 checkSum;                    ///< Image checksum (used by the loader for validation of drivers/system DLLs).
    u16 subsystem;                   ///< Target subsystem (e.g. Windows GUI = 2, Windows CUI = 3).
    u16 dllCharacteristics;          ///< DLL characteristics flags (e.g. ASLR, DEP/NX, CFG).
    u64 sizeOfStackReserve;          ///< Stack size to reserve for the main thread.
    u64 sizeOfStackCommit;           ///< Stack size to commit for the main thread.
    u64 sizeOfHeapReserve;           ///< Default heap size to reserve.
    u64 sizeOfHeapCommit;            ///< Default heap size to commit.
    u32 loaderFlags;                 ///< Reserved, must be 0.
    u32 numberOfRvaAndSizes;         ///< Number of valid entries in dataDirectory (usually 16).
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
struct PACKED IMAGE_NT_HEADERS64 {
    u32 signature;                         ///< PE signature: 0x00004550 (ASCII "PE\0\0").
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
struct PACKED IMAGE_SECTION_HEADER {
    u8  name[8];                  ///< Null-padded section name (e.g. ".text", ".data").
    u32 virtualSize;              ///< Size of the section in memory (analogous to p_memsz).
    u32 virtualAddress;           ///< RVA of the section in memory (add imageBase for a full VA).
    u32 sizeOfRawData;            ///< Size of the section on disk (analogous to p_filesz).
    u32 pointerToRawData;         ///< File offset of the section's raw data (analogous to p_offset).
    u32 pointerToRelocations;     ///< File offset of relocations for this section (object files only).
    u32 pointerToLinenumbers;     ///< File offset of line-number entries (deprecated, COFF debug info).
    u16 numberOfRelocations;      ///< Number of relocation entries for this section.
    u16 numberOfLinenumbers;      ///< Number of line-number entries for this section.
    u32 characteristics;          ///< Section flags (analogous to p_flags): executable, readable, writable, etc.
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
u64 pe_load(const char* path, u64 *pml4);

#endif // PE_H

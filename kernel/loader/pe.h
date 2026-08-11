#ifndef PE_H
#define PE_H

#include <stdint.h>

// Equivalent to the start of your Elf64_Ehdr, but legacy DOS junk
struct __attribute__((packed)) IMAGE_DOS_HEADER {
    uint16_t e_magic;      // 0x5A4D ("MZ")
    uint16_t e_cblp;
    uint16_t e_cp;
    uint16_t e_crlc;
    uint16_t e_cparhdr;
    uint16_t e_minalloc;
    uint16_t e_maxalloc;
    uint16_t e_ss;
    uint16_t e_sp;
    uint16_t e_csum;
    uint16_t e_ip;
    uint16_t e_cs;
    uint16_t e_lfarlc;
    uint16_t e_ovno;
    uint16_t e_res[4];
    uint16_t e_oemid;
    uint16_t e_oeminfo;
    uint16_t e_res2[10];
    uint32_t e_lfanew;     // Offset to the PE Header (IMAGE_NT_HEADERS64)
};

struct __attribute__((packed)) IMAGE_FILE_HEADER {
    uint16_t machine;              // 0x8664 for x64
    uint16_t numberOfSections;     // How many Phdr-equivalents we have
    uint32_t timeDateStamp;
    uint32_t pointerToSymbolTable;
    uint32_t numberOfSymbols;
    uint16_t sizeOfOptionalHeader; // Size of the next struct
    uint16_t characteristics;
};

struct __attribute__((packed)) IMAGE_DATA_DIRECTORY {
    uint32_t virtualAddress;
    uint32_t size;
};

struct __attribute__((packed)) IMAGE_OPTIONAL_HEADER64 {
    uint16_t magic;                       // 0x20B for PE32+
    uint8_t  majorLinkerVersion;
    uint8_t  minorLinkerVersion;
    uint32_t sizeOfCode;
    uint32_t sizeOfInitializedData;
    uint32_t sizeOfUninitializedData;
    uint32_t addressOfEntryPoint;         // Equivalent to e_entry
    uint32_t baseOfCode;
    uint64_t imageBase;                   // Preferred load address
    uint32_t sectionAlignment;            // Usually 0x1000
    uint32_t fileAlignment;               // Usually 0x200
    uint16_t majorOperatingSystemVersion;
    uint16_t minorOperatingSystemVersion;
    uint16_t majorImageVersion;
    uint16_t minorImageVersion;
    uint16_t majorSubsystemVersion;
    uint16_t minorSubsystemVersion;
    uint32_t win32VersionValue;
    uint32_t sizeOfImage;                 // Total RAM to allocate
    uint32_t sizeOfHeaders;               // Size of DOS+NT+Sections on disk
    uint32_t checkSum;
    uint16_t subsystem;
    uint16_t dllCharacteristics;
    uint64_t sizeOfStackReserve;
    uint64_t sizeOfStackCommit;
    uint64_t sizeOfHeapReserve;
    uint64_t sizeOfHeapCommit;
    uint32_t loaderFlags;
    uint32_t numberOfRvaAndSizes;
    struct IMAGE_DATA_DIRECTORY dataDirectory[16]; // Imports, Exports, Relocs go here
}; // Size: 240 bytes

// The main PE Header (what e_lfanew points to)
struct __attribute__((packed)) IMAGE_NT_HEADERS64 {
    uint32_t signature; // 0x00004550 ("PE\0\0")
    struct IMAGE_FILE_HEADER fileHeader;
    struct IMAGE_OPTIONAL_HEADER64 optionalHeader;
};

// Equivalent to your Elf64_Phdr
struct __attribute__((packed)) IMAGE_SECTION_HEADER {
    uint8_t  name[8];             // e.g., ".text", ".data"
    uint32_t virtualSize;         // Equivalent to p_memsz
    uint32_t virtualAddress;      // Equivalent to p_vaddr (but an RVA, so add ImageBase)
    uint32_t sizeOfRawData;       // Equivalent to p_filesz
    uint32_t pointerToRawData;    // Equivalent to p_offset
    uint32_t pointerToRelocations;
    uint32_t pointerToLinenumbers;
    uint16_t numberOfRelocations;
    uint16_t numberOfLinenumbers;
    uint32_t characteristics;     // Equivalent to p_flags (Executable, Writable, Readable)
}; // Size: 40 bytes

uint64_t pe_load(const char* path, uint64_t *pml4);

#endif // PE_H

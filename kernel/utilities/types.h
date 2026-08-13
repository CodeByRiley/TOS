/* kernel/utilities/types.h — kernel typedef bag.
 *
 * Collects every alias the kernel codebase uses: POSIX names, fixed-width
 * shorthands (u8/u32, i8/i32, uint8/uint32), pointer-sized integers, and
 * address-space markers (paddr_t / vaddr_t). One header so the same names
 * resolve consistently across every translation unit.
 *
 * Also hosts a few generic helper structs (M4_i, M4_f, Process,
 * MemoryBlock, StatusCode) used by ad-hoc code paths.
 */
#ifndef TYPES_H
#define TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- Linux-style short names --------------------------------------- */
typedef unsigned char  uchar;
typedef unsigned short ushort;
typedef unsigned int   uint;
typedef unsigned long  ulong;
typedef long long      llong;

/* --- Doom-style fixed-width aliases -------------------------------- */
typedef int8_t   i8;
typedef uint8_t  ui8;
typedef int16_t  i16;
typedef uint16_t ui16;
typedef int32_t  i32;
typedef uint32_t ui32;
typedef int64_t  i64;
typedef uint64_t ui64;

/* --- Single-letter unsigned aliases -------------------------------- */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

/* --- "intN"-style aliases ------------------------------------------ */
typedef int8_t   int8;
typedef uint8_t  uint8;
typedef int16_t  int16;
typedef uint16_t uint16;
typedef int32_t  int32;
typedef uint32_t uint32;
typedef int64_t  int64;
typedef uint64_t uint64;

/* --- Pointer-sized + size types ------------------------------------ */
typedef intptr_t  iptr;
typedef uintptr_t uptr;
typedef size_t    usize;
typedef intptr_t  isize;

typedef const char* str;

/* --- Address-space markers (semantic-only) ------------------------- */
typedef uintptr_t addr_t;
typedef uintptr_t paddr_t;
typedef uintptr_t vaddr_t;

typedef uint8_t byte;

/* --- Lowercase bool alias used by some legacy code paths ----------- */
typedef enum boolean {
    FALSE = 0,
    TRUE  = 1,
} boolean;

typedef void (*func_ptr)(void);

/* --- Helper structs ------------------------------------------------ */
typedef struct M4_i {
    int elements[4][4];
} M4_i;

typedef struct M4_f {
    float elements[4][4];
} M4_f;

typedef struct Process {
    int  pid;
    char name[256];
    int  priority;
} Process;

typedef struct MemoryBlock {
    void  *address;
    size_t size;
} MemoryBlock;

typedef enum {
    SUCCESS       = 0,
    ERROR         = 1,
    TIMEOUT       = 2,
    INVALID_PARAM = 3,
} StatusCode;

#endif

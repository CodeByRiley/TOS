/* kernel/utilities/types.h , kernel typedef bag.
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

/* --- Compiler Attributes & Macros --------------------------------- */
#define PACKED __attribute__((packed))
#define ALIGNED(x) __attribute__((aligned(x)))
#define NORETURN __attribute__((noreturn))
#define UNUSED __attribute__((unused))
#define WEAK __attribute__((weak))

#define SINLINE static inline

/* Branch prediction hints for performance-critical paths */
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

/* Section placement (useful for linking init code, per-CPU data, etc.) */
#define SECTION(name) __attribute__((section(name)))

/* For declaring aliases or preventing strict aliasing optimisations */
#define MAY_ALIAS __attribute__((may_alias))

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


/* --- Linux-style short names --------------------------------------- */
typedef unsigned char uchar;
typedef unsigned short ushort;
typedef unsigned int uint;
typedef unsigned long ulong;
typedef long long llong;

/* --- Doom-style fixed-width aliases -------------------------------- */
typedef int8_t i8;
typedef uint8_t ui8;
typedef int16_t i16;
typedef uint16_t ui16;
typedef int32_t i32;
typedef uint32_t ui32;
typedef int64_t i64;
typedef uint64_t ui64;

/* --- Single-letter unsigned aliases -------------------------------- */
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

/* --- "intN"-style aliases ------------------------------------------ */
typedef int8_t int8;
typedef u8 uint8;
typedef int16_t int16;
typedef u16 uint16;
typedef int32_t int32;
typedef u32 uint32;
typedef int64_t int64;
typedef u64 uint64;

/* --- Pointer-sized + size types ------------------------------------ */
typedef intptr_t iptr;
typedef uintptr_t uptr;
typedef size_t usize;
typedef intptr_t isize;

/* Register-sized type (used for register dumps) */
typedef uintptr_t reg_t;

typedef const char *str;

/* --- Address-space markers (semantic-only) ------------------------- */
typedef uintptr_t addr_t;
typedef uintptr_t paddr_t;
typedef uintptr_t vaddr_t;

/* Page Frame Number (physical address >> PAGE_SHIFT) */
typedef uintptr_t pfn_t;

/* I/O port address (x86 specific, but useful to define globally) */
typedef u16 ioport_t;

typedef u8 byte;

/* --- Lowercase bool alias used by some legacy code paths ----------- */
typedef enum boolean {
  FALSE = 0,
  TRUE = 1,
} boolean;

typedef void (*func_ptr)(void);

/* --- OS-Specific Subsystem Handles --------------------------------- */
typedef int pid_t;       /* Process ID */
typedef int dev_t;       /* Device ID */
typedef int fd_t;        /* File Descriptor */
typedef int64_t off_t;   /* File offset */
typedef u64 tick_t; /* System timer ticks */

/* --- Helper structs ------------------------------------------------ */
typedef struct M4_i {
  i32 elements[4][4]; /* Changed int to i32 for cross-arch consistency */
} M4_i;

typedef struct M4_f {
  float elements[4][4];
} M4_f;


/* --- Common Helper Macros ------------------------------------------ */
#ifndef NULL
#define NULL ((void *)0)
#endif

/* Array element count */
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

/* Bitmask creation */
#define BIT(n) (1ULL << (n))

/* Alignment math */
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))

/* Offset of a member within a struct */
#define OFFSET_OF(type, member) __builtin_offsetof(type, member)

/* Container_of macro (crucial for intrusive linked lists in kernels) */
#define CONTAINER_OF(ptr, type, member)                                        \
  ((type *)((char *)(ptr) - OFFSET_OF(type, member)))

#endif

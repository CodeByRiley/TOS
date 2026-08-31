/* userspace/include/sys/types.h , typedef bag.
 *
 * Collects every alias different bits of the codebase like to use:
 * the POSIX names (ssize_t, off_t, pid_t...), the short ones (u8, u32...),
 * the address types (addr_t / paddr_t / vaddr_t), and the legacy Doom
 * shorthands. Keeping them in one header avoids drift when something
 * gets included from multiple translation units.
 */
#ifndef SYS_TYPES_H
#define SYS_TYPES_H

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
#include <stdint.h>
#include <stddef.h>

/* --- Linux-style short names ------------------------------------------ */
typedef unsigned char  uchar;
typedef unsigned short ushort;
typedef unsigned int   uint;
typedef unsigned long  ulong;
typedef long long      llong;

/* --- Doom-style fixed-width aliases ----------------------------------- */
typedef int8_t   i8;
typedef uint8_t  ui8;
typedef int16_t  i16;
typedef uint16_t ui16;
typedef int32_t  i32;
typedef uint32_t ui32;
typedef int64_t  i64;
typedef uint64_t ui64;

/* --- Single-letter unsigned aliases ----------------------------------- */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

/* --- "intN"-style aliases without underscore -------------------------- */
typedef int8_t   int8;
typedef uint8_t  uint8;
typedef int16_t  int16;
typedef uint16_t uint16;
typedef int32_t  int32;
typedef uint32_t uint32;
typedef int64_t  int64;
typedef uint64_t uint64;

/* --- Pointer-sized + size types --------------------------------------- */
typedef intptr_t  iptr;
typedef uintptr_t uptr;
typedef size_t    usize;
typedef intptr_t  isize;

/* --- Address spaces (mostly kernel-facing) ---------------------------- */
typedef uintptr_t addr_t;
typedef uintptr_t paddr_t;
typedef uintptr_t vaddr_t;

/* --- POSIX scalar types ---------------------------------------------- */
#ifndef _SSIZE_T_DEFINED
typedef long ssize_t;
#define _SSIZE_T_DEFINED
#endif
#ifndef _TIME_T_DEFINED
typedef long time_t;
#define _TIME_T_DEFINED
#endif
#ifndef OFF_T
typedef long          off_t;
#endif
typedef int           pid_t;
typedef unsigned int  mode_t;
typedef unsigned int  uid_t;
typedef unsigned int  gid_t;

typedef void (*func_ptr)(void);

/* --- OS-Specific Subsystem Handles --------------------------------- */
typedef int pid_t;       /* Process ID */
typedef int dev_t;       /* Device ID */
typedef int fd_t;        /* File Descriptor */
typedef int64_t off_t;   /* File offset */
typedef u64 tick_t; /* System timer ticks */


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

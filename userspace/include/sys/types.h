#ifndef SYS_TYPES_H
#define SYS_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef unsigned char  uchar;
typedef unsigned short ushort;
typedef unsigned int   uint;
typedef unsigned long  ulong;
typedef long long      llong;

typedef int8_t   i8;
typedef uint8_t  ui8;
typedef int16_t  i16;
typedef uint16_t ui16;
typedef int32_t  i32;
typedef uint32_t ui32;
typedef int64_t  i64;
typedef uint64_t ui64;

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t   int8;
typedef uint8_t  uint8;
typedef int16_t  int16;
typedef uint16_t uint16;
typedef int32_t  int32;
typedef uint32_t uint32;
typedef int64_t  int64;
typedef uint64_t uint64;

typedef intptr_t  iptr;
typedef uintptr_t uptr;
typedef size_t    usize;
typedef intptr_t  isize;

typedef uintptr_t addr_t;
typedef uintptr_t paddr_t;
typedef uintptr_t vaddr_t;

typedef long          ssize_t;
typedef long          off_t;
typedef int           pid_t;
typedef unsigned int  mode_t;
typedef unsigned int  uid_t;
typedef unsigned int  gid_t;
typedef long          time_t;

#endif

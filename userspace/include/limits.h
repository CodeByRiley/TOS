/* userspace/include/limits.h — integer-range macros.
 *
 * Assumes a 64-bit LP64-ish target (long == 64 bits). char is 8 bits,
 * short is 16, int is 32. Add new entries here when a ported library
 * starts asking for them.
 */
#ifndef LIMITS_H
#define LIMITS_H

#define INT_MAX     2147483647
#define INT_MIN     (-INT_MAX-1)
#define UINT_MAX    4294967295U
#define LONG_MAX    9223372036854775807L
#define LONG_MIN    (-LONG_MAX-1)
#define CHAR_BIT    8
#define SCHAR_MAX   127
#define SCHAR_MIN   (-128)
#define UCHAR_MAX   255
#define SHRT_MAX    32767
#define USHRT_MAX   65535U
#define UINT_MAX    4294967295U

#endif

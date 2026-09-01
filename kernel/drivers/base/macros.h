#ifndef DRIVER_MACROS_H
#define DRIVER_MACROS_H

#include <stdint.h>

#define FIELD_SHIFT(mask) \
    ((unsigned)__builtin_ctz((uint32_t)(mask)))

#define FIELD_MASK(width, shift) \
    ((uint32_t)((((uint32_t)1u << (width)) - 1u) << (shift)))

#define FIELD_GET(mask, reg) \
    ((((uint32_t)(reg)) & (uint32_t)(mask)) >> FIELD_SHIFT(mask))

#define FIELD_PREP(mask, value) \
    ((((uint32_t)(value)) << FIELD_SHIFT(mask)) & (uint32_t)(mask))

#endif /* DRIVER_MACROS_H */

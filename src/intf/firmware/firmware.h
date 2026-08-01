#ifndef FIRMWARE_H
#define FIRMWARE_H

#include <stdint.h>

/* Load a root-filesystem file into kernel-owned memory.  The caller owns a
 * successful result and must release it with firmware_release(). */
int  firmware_load(const char *path, const void **data, uint64_t *size);
void firmware_release(const void *data);

#endif

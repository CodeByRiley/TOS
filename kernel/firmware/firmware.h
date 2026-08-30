#ifndef FIRMWARE_H
#define FIRMWARE_H

#include <stdint.h>
#include <utilities/types.h>

/* Load a root-filesystem file into kernel-owned memory.  The caller owns a
 * successful result and must release it with firmware_release(). */
int  firmware_load(const char *path, const void **data, u64 *size);
void firmware_release(const void *data);

#endif

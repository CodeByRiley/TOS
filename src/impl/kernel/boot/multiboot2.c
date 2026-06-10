/* src/impl/kernel/boot/multiboot2.c — Multiboot2 tag walk helpers.
 *
 * Linear walk over the tag chain GRUB hands us at boot. Tags are 8-byte
 * aligned; the chain ends with MULTIBOOT_TAG_END.
 */
#include "boot/multiboot2.h"
#include "utilities/string.h"

/* Walk the tag chain looking for one of the given `type`. NULL on miss. */
struct MB2_TAG *mb2_find_tag(uint64_t mb2_addr, uint32_t type) {
    uint8_t *p = (uint8_t*)mb2_addr + 8;     /* skip 8-byte header */
    while (1) {
        struct MB2_TAG *t = (struct MB2_TAG*)p;
        if (t->type == MULTIBOOT_TAG_END) return 0;
        if (t->type == type) return t;
        p += (t->size + 7) & ~7;             /* 8-byte align */
    }
}

/* Walk module tags. If `cmdline` is NULL, returns the first module;
 * otherwise returns the first module whose cmdline matches exactly. */
struct MB2_TAG_MODULE *mb2_find_module(uint64_t mb2_addr, const char *cmdline) {
    uint8_t *p = (uint8_t*)mb2_addr + 8;
    while (1) {
        struct MB2_TAG *t = (struct MB2_TAG*)p;
        if (t->type == MULTIBOOT_TAG_END) return 0;
        if (t->type == MULTIBOOT_TAG_MODULE) {
            struct MB2_TAG_MODULE *m = (struct MB2_TAG_MODULE*)t;
            if (!cmdline || strcmp(m->cmdline, cmdline) == 0) return m;
        }
        p += (t->size + 7) & ~7;
    }
}

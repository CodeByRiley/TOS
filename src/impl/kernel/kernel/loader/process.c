#include "utilities/string.h"
#include "utilities/log.h"
#include "loader/process.h"
#include "loader/elf.h"
#include "memory/heap.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include <stdint.h>

#define ARGV_MAX 16
#define ARG_LEN_MAX 128

#define ADDR_MASK 0x000FFFFFFFFFF000ULL
#define USER_PML4_ENTRIES 256

extern uint64_t *kernel_pml4;
extern long enter_user(uint64_t entry, uint64_t user_rsp);
extern uint64_t kernel_rsp_top;        /* SYSCALL kernel stack top, set by syscall_init */

/* Per-nesting-depth kernel stacks. SYSCALL stub clobbers fixed kernel_rsp_top
 * each entry; a nested user (child) would overwrite the parent's saved
 * RCX/R11 at that location. Each process_exec switches to a private stack so
 * child syscalls can't corrupt parent's. */
#define MAX_NESTED_STACKS  8
#define NESTED_STACK_BYTES 16384

static uint8_t  nested_kstacks[MAX_NESTED_STACKS][NESTED_STACK_BYTES]
    __attribute__((aligned(16)));
static int      nested_depth = 0;

struct user_mapping_snapshot {
    uint64_t pml4_0_pdpt[511];
    uint64_t pml4_entries[USER_PML4_ENTRIES - 1];
};

void user_stack_alloc(void) {
    for (int i = 0; i < USER_STACK_PAGES; i++) {
        uint64_t phys = pmm_alloc_frame();
        memset((void*)phys, 0, 4096);
        vmm_map(USER_STACK_TOP - (i + 1) * 4096, phys,
                VMM_PRESENT | VMM_WRITE | VMM_USER);
    }
}

static void flush_tlb(void) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3));
}

/* Save lower-half user mappings. PML4[0]/PDPT[0] is the boot identity map
 * used by the kernel, so keep that entry live and only swap out user entries. */
static void save_user_mappings(struct user_mapping_snapshot *out) {
    uint64_t pml4_0 = kernel_pml4[0];
    if (!(pml4_0 & 1)) {
        for (int i = 0; i < 511; i++) out->pml4_0_pdpt[i] = 0;
    } else {
        uint64_t *pdpt = (uint64_t*)(pml4_0 & ADDR_MASK);
        for (int i = 0; i < 511; i++) out->pml4_0_pdpt[i] = pdpt[i + 1];
    }

    for (int i = 1; i < USER_PML4_ENTRIES; i++) {
        out->pml4_entries[i - 1] = kernel_pml4[i];
    }
}

static void clear_user_mappings(void) {
    uint64_t pml4_0 = kernel_pml4[0];
    if (pml4_0 & 1) {
        uint64_t *pdpt = (uint64_t*)(pml4_0 & ADDR_MASK);
        for (int i = 1; i < 512; i++) pdpt[i] = 0;
    }

    for (int i = 1; i < USER_PML4_ENTRIES; i++) {
        kernel_pml4[i] = 0;
    }

    flush_tlb();
}

static void restore_user_mappings(const struct user_mapping_snapshot *in) {
    uint64_t pml4_0 = kernel_pml4[0];
    if (pml4_0 & 1) {
        uint64_t *pdpt = (uint64_t*)(pml4_0 & ADDR_MASK);
        for (int i = 0; i < 511; i++) pdpt[i + 1] = in->pml4_0_pdpt[i];
    }

    for (int i = 1; i < USER_PML4_ENTRIES; i++) {
        kernel_pml4[i] = in->pml4_entries[i - 1];
    }

    flush_tlb();
}

long process_exec(const char *path, char *const argv[]) {
    /* 1a. snapshot path into kernel heap (caller-supplied pointer lives in
     *     user pages that we are about to unmap). */
    char saved_path[ARG_LEN_MAX];
    {
        size_t i = 0;
        if (path) {
            while (i < ARG_LEN_MAX - 1 && path[i]) {
                saved_path[i] = path[i];
                i++;
            }
        }
        saved_path[i] = 0;
    }

    /* 1b. snapshot argv into kernel heap. */
    int    argc = 0;
    char  *saved[ARGV_MAX] = {0};
    if (argv) {
        while (argc < ARGV_MAX && argv[argc]) {
            size_t len = 0;
            const char *s = argv[argc];
            while (s[len] && len < ARG_LEN_MAX - 1) len++;
            char *copy = (char*)kmalloc(len + 1);
            if (!copy) break;
            memcpy(copy, s, len);
            copy[len] = 0;
            saved[argc++] = copy;
        }
    }

    /* 2. swap out caller's user mappings */
    struct user_mapping_snapshot save;
    save_user_mappings(&save);
    clear_user_mappings();

    /* 3. load child ELF + alloc child stack (use saved_path, not user path) */
    uint64_t entry = elf_load(saved_path);
    if (!entry) {
        log_write("process_exec: elf load failed", KERNEL, LOG_ERROR);
        for (int i = 0; i < argc; i++) kfree(saved[i]);
        restore_user_mappings(&save);
        return -1;
    }
    user_stack_alloc();

    /* 4. lay out argv on child stack
     *    layout (top-down): strings, NULL sentinel, argv[argc-1]..argv[0], argc
     *    final RSP points at argc; argv[i] at [rsp + 8 + i*8]
     */
    uint64_t rsp   = USER_STACK_TOP;
    uint64_t pstr[ARGV_MAX];

    for (int i = 0; i < argc; i++) {
        size_t len = strlen(saved[i]) + 1;
        rsp -= len;
        memcpy((void*)rsp, saved[i], len);
        pstr[i] = rsp;
        kfree(saved[i]);
    }
    rsp &= ~0xFULL;                          /* 16-byte align */

    rsp -= 8; *(uint64_t*)rsp = 0;            /* argv[argc] = NULL */
    for (int i = argc - 1; i >= 0; i--) {
        rsp -= 8;
        *(uint64_t*)rsp = pstr[i];
    }
    rsp -= 8; *(uint64_t*)rsp = (uint64_t)argc;
    if (rsp & 0xF) rsp -= 8;                  /* keep 16-byte align */

    /* 5. swap to a fresh kernel SYSCALL stack so child syscalls don't stomp
     *    parent's saved RCX/R11/regs at the previous kernel_rsp_top. */
    uint64_t saved_top = kernel_rsp_top;
    int my_slot = nested_depth++;
    if (my_slot >= MAX_NESTED_STACKS) {
        log_write("process_exec: nested-stack depth exhausted", KERNEL, LOG_ERROR);
        nested_depth--;
        restore_user_mappings(&save);
        return -1;
    }
    kernel_rsp_top = (uint64_t)(nested_kstacks[my_slot] + NESTED_STACK_BYTES);

    /* 6. enter ring 3 */
    long code = enter_user(entry, rsp);

    /* 7. restore parent's syscall stack + user mappings */
    kernel_rsp_top = saved_top;
    nested_depth--;
    restore_user_mappings(&save);
    return code;
}

/* src/impl/kernel/loader/process.c — process create / exec / spawn.
 *
 * Orchestrates the steps to launch a user-mode task:
 *   1. process_pml4_create()    — fresh PML4 sharing kernel-low identity
 *   2. user_stack_alloc_in()    — populate the user stack pages
 *   3. elf_load()               — load the ELF into the PML4
 *   4. argv marshal             — copy argv strings + pointer array onto
 *                                  the user stack
 *   5. task_spawn_user()        — queue the new task ready to run
 *
 * process_exec blocks until the child exits and returns its code.
 * process_spawn_async returns the child's pid immediately so daemons
 * (winman) can run alongside a foreground shell.
 */
#include "utilities/string.h"
#include "utilities/log.h"
#include "loader/process.h"
#include "loader/elf.h"
#include "memory/heap.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "sched/sched.h"
#include <stdint.h>

#define ARGV_MAX 16
#define ARG_LEN_MAX 128

#define ADDR_MASK 0x000FFFFFFFFFF000ULL
#define PAGE_PS   (1ULL << 7)

extern uint64_t *kernel_pml4;

static inline uint64_t read_cr3(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline void write_cr3(uint64_t v) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(v) : "memory");
}

/* Walk one PDPT subtree and free every present user frame underneath, plus
 * each level's table. `start_idx` skips entries (used to leave the kernel-low
 * identity map intact under PML4[0]'s PDPT[0]). Always frees the PDPT frame
 * itself when done. The previous version copy-pasted this loop twice; one
 * day someone would have "fixed" one half and not the other. */
static void free_pdpt_subtree(uint64_t pdpt_phys, int start_idx) {
    uint64_t *pdpt = (uint64_t*)pdpt_phys;
    for (int i = start_idx; i < 512; i++) {
        uint64_t pdpt_e = pdpt[i];
        if (!(pdpt_e & VMM_PRESENT) || (pdpt_e & PAGE_PS)) continue;
        uint64_t pd_phys = pdpt_e & ADDR_MASK;
        uint64_t *pd = (uint64_t*)pd_phys;
        for (int j = 0; j < 512; j++) {
            uint64_t pd_e = pd[j];
            if (!(pd_e & VMM_PRESENT) || (pd_e & PAGE_PS)) continue;
            uint64_t pt_phys = pd_e & ADDR_MASK;
            uint64_t *pt = (uint64_t*)pt_phys;
            for (int k = 0; k < 512; k++) {
                uint64_t pte = pt[k];
                if (!(pte & VMM_PRESENT)) continue;
                /* Skip shmem-borrowed frames: owner still holds them and
                 * will recycle via its own free()/munmap. Double-freeing
                 * here lets the next pmm_alloc_frame hand them back out
                 * while the owner still writes to them, leading to a
                 * page-fault about two seconds and one panic later. */
                if (pte & VMM_SHARED) continue;
                pmm_free_frame(pte & ADDR_MASK);
            }
            pmm_free_frame(pt_phys);
        }
        pmm_free_frame(pd_phys);
    }
    pmm_free_frame(pdpt_phys);
}

/* Walk a process PML4 and free every page it uses (page tables + user
 * frames in the lower half + the PML4 frame itself). PDPT[0] under PML4[0]
 * is the kernel-shared low 1 GiB identity map: skip walking that subtree
 * but still free the per-process PDPT frame. PML4[256..511] is also shared
 * (kernel high half) so leave those alone. Called from sched.c::task_reap. */
void free_user_pml4(uint64_t *pml4) {
    if (!pml4) return;

    uint64_t pml4_0 = pml4[0];
    if (pml4_0 & VMM_PRESENT) {
        free_pdpt_subtree(pml4_0 & ADDR_MASK, /*start_idx=*/1);
    }

    for (int p = 1; p < 256; p++) {
        uint64_t pml4_e = pml4[p];
        if (!(pml4_e & VMM_PRESENT)) continue;
        free_pdpt_subtree(pml4_e & ADDR_MASK, /*start_idx=*/0);
    }

    pmm_free_frame((uint64_t)pml4);
}

void user_stack_alloc(void) {
    user_stack_alloc_in(kernel_pml4);
}

void user_stack_alloc_in(uint64_t *pml4) {
    for (int i = 0; i < USER_STACK_PAGES; i++) {
        uint64_t phys = pmm_alloc_frame();
        memset((void*)phys, 0, 4096);
        vmm_map_in(pml4, USER_STACK_TOP - (i + 1) * 4096, phys,
                   VMM_PRESENT | VMM_WRITE | VMM_USER);
    }
}

uint64_t *process_pml4_create(void) {
    uint64_t pml4_phys = pmm_alloc_frame();
    if (!pml4_phys) return 0;
    uint64_t *pml4 = (uint64_t*)pml4_phys;
    memset(pml4, 0, 4096);

    /* Each process gets a private PDPT under PML4[0]. PDPT[0] is copied
     * from the kernel's PML4[0]'s PDPT[0] so the kernel-low identity map
     * (first 1 GiB) is shared. PDPT[1..511] starts empty for private
     * user mappings. */
    uint64_t pdpt_phys = pmm_alloc_frame();
    if (!pdpt_phys) {
        pmm_free_frame(pml4_phys);
        return 0;
    }
    uint64_t *pdpt = (uint64_t*)pdpt_phys;
    memset(pdpt, 0, 4096);

    uint64_t kernel_pml4_0 = kernel_pml4[0];
    if (kernel_pml4_0 & 1) {
        uint64_t *kernel_pdpt = (uint64_t*)(kernel_pml4_0 & ADDR_MASK);
        pdpt[0] = kernel_pdpt[0];
    }

    pml4[0] = pdpt_phys | VMM_PRESENT | VMM_WRITE | VMM_USER;

    /* Share kernel high half (entries 256..511). Currently empty, but the
     * clone means future kernel allocations there are visible from every
     * process without per-process bookkeeping. */
    for (int i = 256; i < 512; i++) {
        pml4[i] = kernel_pml4[i];
    }

    return pml4;
}

/* Common front-end shared by process_exec (synchronous) and
 * process_spawn_async (fire-and-forget). Returns the spawned child's pid
 * or -1 on failure. Caller decides whether to block on the result. */
static int process_spawn_common(const char *path, char *const argv[]) {
    /* Snapshot path into kernel-side storage. Caller-supplied pointer lives
     * in user pages we're about to swap out of CR3, and reading from a not-
     * currently-mapped user va is a fast way to learn what #PF feels like. */
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

    // snapshot argv into kernel heap
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

    uint64_t *child_pml4 = process_pml4_create();
    if (!child_pml4) {
        log_write("process_spawn: pml4 alloc failed", KERNEL, LOG_ERROR);
        for (int i = 0; i < argc; i++) kfree(saved[i]);
        return -1;
    }

    uint64_t parent_cr3 = read_cr3();
    write_cr3((uint64_t)child_pml4);

    uint64_t entry = elf_load(saved_path, child_pml4);
    if (!entry) {
        log_write("process_spawn: elf load failed", KERNEL, LOG_ERROR);
        write_cr3(parent_cr3);
        free_user_pml4(child_pml4);
        for (int i = 0; i < argc; i++) kfree(saved[i]);
        return -1;
    }
    user_stack_alloc_in(child_pml4);

    uint64_t user_rsp = USER_STACK_TOP;
    uint64_t pstr[ARGV_MAX];

    for (int i = 0; i < argc; i++) {
        size_t len = strlen(saved[i]) + 1;
        user_rsp -= len;
        memcpy((void*)user_rsp, saved[i], len);
        pstr[i] = user_rsp;
        kfree(saved[i]);
    }
    user_rsp &= ~0xFULL;                           /* 16-byte align */

    user_rsp -= 8; *(uint64_t*)user_rsp = 0;       /* argv[argc] = NULL */
    for (int i = argc - 1; i >= 0; i--) {
        user_rsp -= 8;
        *(uint64_t*)user_rsp = pstr[i];
    }
    user_rsp -= 8; *(uint64_t*)user_rsp = (uint64_t)argc;
    if (user_rsp & 0xF) user_rsp -= 8;             /* keep 16-byte align */

    write_cr3(parent_cr3);

    int parent_pid = task_current()->pid;
    struct task *child = task_spawn_user(child_pml4, entry, user_rsp,
                                         parent_pid);
    if (!child) {
        log_write("process_spawn: task_spawn_user failed", KERNEL, LOG_ERROR);
        free_user_pml4(child_pml4);
        return -1;
    }

    /* Derive a display name from the basename of the ELF path, stripping
     * directory components and the trailing extension. "BIN/SH.ELF" -> "sh". */
    const char *base = saved_path;
    for (const char *p = saved_path; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    char name[16];
    size_t ni = 0;
    while (ni < sizeof(name) - 1 && base[ni] && base[ni] != '.') {
        char c = base[ni];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        name[ni] = c;
        ni++;
    }
    name[ni] = 0;
    task_set_name(child, name);

    return child->pid;
}

long process_exec(const char *path, char *const argv[]) {
		log_write_string("spawning process ", path, KERNEL, LOG_INFO);
    int child_pid = process_spawn_common(path, argv);
    if (child_pid < 0) return -1;

    task_block(child_pid);

    // resumed by child's task_exit. Pull the exit code, reap, return.
    struct task *t = task_find(child_pid);
    long code = -1;
    if (t) {
        code = t->exit_code;
        task_reap(t);
    }
    return code;
}

long process_spawn_async(const char *path, char *const argv[]) {
    return process_spawn_common(path, argv);
}

/* src/intf/loader/process.h — process spawn surface.
 *
 * Wraps PML4 creation, user-stack allocation, ELF load, and task spawn
 * into one entry point. exec() blocks the caller until the child exits;
 * spawn_async() returns the child's pid immediately without waiting.
 *
 * Implementation: src/impl/kernel/loader/process.c.
 */
#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>

#define USER_STACK_TOP   0x00007FFFFFFFE000ULL
#define USER_STACK_PAGES 512

/* Allocate user stack in the current PML4 / in a supplied PML4. */
void user_stack_alloc(void);
void user_stack_alloc_in(uint64_t *pml4);

/* Synchronous exec: blocks until the child exits and returns its code. */
long process_exec(const char *path, char *const argv[]);

/* Fire-and-forget spawn: returns child pid (>0) without waiting. Caller
 * has no claim on the exit code; the child reaches zombie state and is
 * reaped by future improvements (currently — no reaper, leaks the slot
 * on exit). Used for daemons like winman that must run alongside a
 * foreground shell. */
long process_spawn_async(const char *path, char *const argv[]);

/* Allocate a fresh PML4 for a new process. Shares the kernel-low
 * identity map (first 1 GiB) by giving each process its own PDPT under
 * PML4[0] whose PDPT[0] entry references the kernel's existing PD.
 * PML4[1..255] starts empty (private user space). PML4[256..511] is
 * copied from kernel_pml4 (kernel high half — currently empty but
 * shared if populated later). Returns the new PML4 va (identity-mapped),
 * or NULL on failure. */
uint64_t *process_pml4_create(void);

#endif

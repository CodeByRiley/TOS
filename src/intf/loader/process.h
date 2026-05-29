#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>

#define USER_STACK_TOP   0x00007FFFFFFFE000ULL
#define USER_STACK_PAGES 512

void user_stack_alloc(void);
void user_stack_alloc_in(uint64_t *pml4);
long process_exec(const char *path, char *const argv[]);

/* Fire-and-forget spawn: returns the child's pid (>0) without blocking
 * the caller. Caller has no claim on the child's exit code; the child
 * is reaped when it goes zombie (no waiter -> never reaped — TODO).
 * Used for daemons like the userspace winman that must run alongside
 * a foreground shell. */
long process_spawn_async(const char *path, char *const argv[]);

/* Allocate a fresh PML4 for a new process. Shares the kernel-low identity
 * map (first 1 GiB) by giving each process its own PDPT under PML4[0] whose
 * PDPT[0] entry references the kernel's existing PD. PML4[1..255] starts
 * empty (private user space). PML4[256..511] is copied from kernel_pml4
 * (kernel high half — currently empty but shared if populated later).
 * Returns physical/virt-identity address of the PML4, 0 on failure. */
uint64_t *process_pml4_create(void);

#endif /* PROCESS_H */

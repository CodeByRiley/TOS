/* kernel/loader/process.h , process spawn surface.
 *
 * Wraps PML4 creation, user-stack allocation, ELF load, and task spawn
 * into one entry point. exec() blocks the caller until the child exits;
 * spawn_async() reserves the final child pid and queues image loading on a
 * kernel worker before returning.
 *
 * Implementation: kernel/loader/process.c.
 */
#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include <utilities/types.h>

/* --- User address-space map -------------------------------------------
 *
 * One place for every fixed user VA boundary. These used to live in four
 * files (elf.c, sched.c, syscall.c, here) with nothing checking they
 * agreed.
 *
 *   0x0000_1000 .. 0x6000_0000   image: ELF PT_LOAD / PE section mapping
 *   0x6000_0000 .. 0x7000_0000   framebuffer window (SYS_FB_MAP)
 *   0x7000_0000 .. 0x8000_0000   mmap arena (auto-placed allocations)
 *   0x8000_0000 .. 1 GiB up      shmem arena (SYS_SHMEM_SHARE, grows up)
 *   0x1_0000_0000 and above      free for MAP_FIXED (PE ImageBase lands
 *                                here: 0x1_4000_0000 for a 64-bit .exe)
 *   USER_STACK_TOP downward      stack, USER_STACK_PAGES eagerly mapped
 */
#define USER_VA_MIN 0x0000000000001000ULL
#define USER_IMAGE_MAX 0x0000000070000000ULL
#define USER_FB_BASE 0x0000000060000000ULL
#define USER_MMAP_BASE 0x0000000070000000ULL
#define USER_MMAP_LIMIT 0x0000000080000000ULL
#define USER_SHMEM_BASE 0x0000000080000000ULL

/* Ceiling for MAP_FIXED. Well clear of the stack, and inside the 47-bit
 * canonical lower half. */
#define USER_VA_MAX 0x00007F0000000000ULL

#define USER_STACK_TOP 0x00007FFFFFFFE000ULL
#define USER_STACK_PAGES 512
#define USER_STACK_LOW (USER_STACK_TOP - (u64)USER_STACK_PAGES * 4096)

/* Allocate a user stack in the kernel PML4 / in a supplied PML4.
 * Returns 0 on success and -1 on allocation or mapping failure. */
int user_stack_alloc(void);
int user_stack_alloc_in(u64 *pml4);

/* Synchronous exec: blocks until the child exits and returns its code. */
long process_exec(const char *path, char *const argv[]);

/* Fire-and-forget spawn: reserves and returns the final child pid (>0), then
 * loads the image on the BSP loader task. The task is visible as LOADING until
 * activation and is reaped normally if loading fails. */
long process_spawn_async(const char *path, char *const argv[]);

/* As process_spawn_async, but pins the child to TTY channel `tty` instead of
 * inheriting the caller's. This is how winman starts a shell on a console it
 * just opened: the child must not land on winman's own channel, and it has
 * to be set before the image runs, not after. Out-of-range or closed
 * channels are rejected with -1 rather than silently falling back to 0. */
long process_spawn_async_tty(const char *path, char *const argv[], int tty);
/* Cancel a queued or in-progress async load. Used by kill(). */
int process_cancel_async(int pid, long code);
long process_kill(long pid, int signal);

/* Allocate a fresh PML4 for a new process. Shares the kernel-low
 * identity map (first 1 GiB) by giving each process its own PDPT under
 * PML4[0] whose PDPT[0] entry references the kernel's existing PD.
 * PML4[1..255] starts empty (private user space). Existing kernel-half
 * entries are copied from kernel_pml4; their lower table levels remain
 * shared. Returns the new PML4 through the HHDM,
 * or NULL on failure. */
u64 *process_pml4_create(void);

#endif

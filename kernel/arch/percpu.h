/* kernel/arch/percpu.h — per-CPU data area.
 *
 * GS_BASE points at the running CPU's struct cpu_local. Kernel code reads
 * and writes per-CPU state without locks or explicit CPU-id passing by
 * dereferencing gs-relative offsets. The first field is a self-pointer so
 * C code can fetch the struct base with a `mov rax, gs:0`.
 *
 * Layout is shared with asm: SYSCALL entry (kernel/arch/x86_64/cpu/syscall.asm)
 * loads kernel_rsp_top and stashes user_rsp_save by hard-coded offsets.
 * Any field reorder must update the offset macros below + the asm. The
 * static_asserts catch silent drift at compile time.
 *
 * The hardware TSS is NOT embedded here — its layout is fixed by the CPU
 * and the GDT descriptor needs its physical address. We hold a pointer to
 * a separately-allocated TSS instead.
 *
 * Implementation: kernel/arch/percpu.c.
 */
#ifndef PERCPU_H
#define PERCPU_H

#include <stddef.h>
#include <stdint.h>

#define MAX_CPUS 8

struct task;
struct tss;

struct cpu_local {
    struct cpu_local *self;             /* gs:0 — for C-level read of base   */
    int               cpu_id;           /* logical id, 0..MAX_CPUS-1         */
    uint8_t           lapic_id;         /* hardware APIC id                  */
    uint8_t           _pad[3];
    uint64_t          kernel_rsp_top;   /* SYSCALL entry: pop here           */
    uint64_t          user_rsp_save;    /* SYSCALL entry: stash user rsp here*/
    struct task      *current;          /* running task on this CPU          */
    struct task      *idle_task;        /* per-CPU idle thread               */
    struct tss       *tss;              /* points into per-CPU TSS array     */
    volatile int      online;           /* 1 once AP has reached scheduler   */
};

/* Asm-visible offsets. Must stay in lockstep with the struct above —
 * SYSCALL entry uses these literally with no symbol lookup. */
#define CPU_LOCAL_KERNEL_RSP_TOP_OFF  16
#define CPU_LOCAL_USER_RSP_SAVE_OFF   24
#define CPU_LOCAL_CURRENT_OFF         32

_Static_assert(offsetof(struct cpu_local, kernel_rsp_top) ==
               CPU_LOCAL_KERNEL_RSP_TOP_OFF,
               "cpu_local.kernel_rsp_top offset is asm-visible");
_Static_assert(offsetof(struct cpu_local, user_rsp_save) ==
               CPU_LOCAL_USER_RSP_SAVE_OFF,
               "cpu_local.user_rsp_save offset is asm-visible");
_Static_assert(offsetof(struct cpu_local, current) ==
               CPU_LOCAL_CURRENT_OFF,
               "cpu_local.current offset is asm-visible");
_Static_assert(sizeof(struct cpu_local) == 64,
               "cpu_local should fit in one cache line");

/* BSP entry: set up the cpu0 slot and wire GS_BASE. */
void              percpu_init_bsp(uint8_t bsp_lapic_id);

/* AP entry: same for an AP slot. */
void              percpu_init_ap(int cpu_id, uint8_t lapic_id);

/* Load GS_BASE on the calling CPU to point at percpu_get(cpu_id). */
void              percpu_arm_gs_this(int cpu_id);

/* Lookup by id / current. */
struct cpu_local *percpu_get(int cpu_id);
struct cpu_local *percpu_this(void);

/* Active CPU count and BSP-only setter (called once after acpi_init). */
int               percpu_cpu_count(void);
void              percpu_set_count(int n);

#endif

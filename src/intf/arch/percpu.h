#ifndef PERCPU_H
#define PERCPU_H

#include <stdint.h>

/* Per-CPU data area. GS_BASE points at the running CPU's struct cpu_local
 * so kernel code can read/write per-CPU state without taking locks or
 * passing a CPU id everywhere. First field is a self-pointer so C code
 * can do `(struct cpu_local *)__builtin_ia32_rdgsbase64()`-style fetches
 * by loading gs:0 into a register.
 *
 * Layout is shared with asm — adding fields means updating the offsets
 * in syscall.asm (and any other asm that reads gs-relative). The bottom
 * three fields (kernel_rsp_top, user_rsp_save, current) are the hot ones
 * that the SYSCALL entry path touches.
 *
 * Don't put the per-CPU TSS *inside* this struct — TSS layout is fixed by
 * the hardware and the GDT descriptor needs its physical address. We keep
 * a pointer to the TSS instead. */

#define MAX_CPUS 8

struct task;
struct tss;

struct cpu_local {
    struct cpu_local *self;             /* gs:0 — for C-level read of base   */
    int               cpu_id;           /* logical id, 0..MAX_CPUS-1         */
    uint8_t           lapic_id;         /* hardware APIC id                  */
    uint8_t           _pad[7];
    uint64_t          kernel_rsp_top;   /* SYSCALL entry: pop here           */
    uint64_t          user_rsp_save;    /* SYSCALL entry: stash user rsp here*/
    struct task      *current;          /* running task on this CPU          */
    struct task      *idle_task;        /* per-CPU idle thread               */
    struct tss       *tss;              /* points into per-CPU TSS array     */
    volatile int      online;           /* 1 once AP has reached scheduler   */
};

/* Asm-visible offsets. Keep these in sync with the struct above. Used by
 * SYSCALL entry to load kernel_rsp_top / stash user_rsp_save without a
 * function call. */
#define CPU_LOCAL_KERNEL_RSP_TOP_OFF  16
#define CPU_LOCAL_USER_RSP_SAVE_OFF   24
#define CPU_LOCAL_CURRENT_OFF         32

void              percpu_init_bsp(uint8_t bsp_lapic_id);
void              percpu_init_ap(int cpu_id, uint8_t lapic_id);
void              percpu_arm_gs_this(int cpu_id);
struct cpu_local *percpu_get(int cpu_id);
struct cpu_local *percpu_this(void);
int               percpu_cpu_count(void);
void              percpu_set_count(int n);

#endif

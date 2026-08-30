/* kernel/arch/gdt.h , kernel GDT + TSS surface.
 *
 * Holds the selectors used by syscall entry / IRET paths and the
 * helpers for installing per-CPU TSSes. AP TSS selectors follow the BSP
 * TSS at +16 byte increments so the AP boot path can compute its own.
 *
 * Implementation: kernel/arch/gdt.c.
 */
#ifndef GDT_H
#define GDT_H

#include <utilities/types.h>
#include <stdint.h>

/* Selectors. CS = code, DS = data; user variants have RPL bits OR'd by
 * the syscall path. */
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA   0x18
#define GDT_USER_CODE   0x20
#define GDT_TSS         0x28      /* BSP TSS (cpu 0). AP TSSes follow at +16 */

/* Compute the TSS selector for an arbitrary CPU id. */
#define GDT_TSS_FOR(cpu) (GDT_TSS + ((cpu) * 16))

void gdt_init(void);

/* Install a TSS descriptor for `cpu_id` at GDT_TSS_FOR(cpu_id). */
void gdt_install_tss(int cpu_id, u64 kstack_top);

/* AP entry: load the BSP-built GDT and the per-CPU TSS for this CPU. */
void gdt_load_tss_this_cpu(int cpu_id);
void gdt_load_this_cpu_full(void);  /* lgdt + segment reload */
void gdt_load_this_cpu(void);       /* compatibility wrapper */

/* Update RSP0 (kernel stack used on ring 3 → ring 0 transitions). */
void tss_set_rsp0(u64 rsp0);
void tss_set_rsp0_for(int cpu_id, u64 rsp0);

#endif

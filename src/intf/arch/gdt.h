#ifndef GDT_H
#define GDT_H

#include <stdint.h>

#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA   0x18
#define GDT_USER_CODE   0x20
#define GDT_TSS         0x28      /* BSP TSS (cpu 0). AP TSSes follow at +16 */

#define GDT_TSS_FOR(cpu) (GDT_TSS + ((cpu) * 16))

void gdt_init(void);
void gdt_install_tss(int cpu_id, uint64_t kstack_top);
void gdt_load_tss_this_cpu(int cpu_id);
void gdt_load_this_cpu_full(void); /* AP: lgdt kernel GDT + reload segments */
void gdt_load_this_cpu(void);      /* compatibility wrapper */
void tss_set_rsp0(uint64_t rsp0);
void tss_set_rsp0_for(int cpu_id, uint64_t rsp0);

#endif

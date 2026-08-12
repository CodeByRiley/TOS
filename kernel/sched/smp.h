/* kernel/sched/smp.h — SMP bring-up.
 *
 * Walks the ACPI-enumerated CPUs and sends INIT-SIPI-SIPI to each AP.
 * Each AP runs `ap_main` once it reaches 64-bit long mode.
 *
 * Implementation: kernel/sched/smp.c.
 */
#ifndef SMP_H
#define SMP_H

#include <stdint.h>

void smp_boot_aps(void);

/* called from ap_trampoline after entering long mode. */
void ap_main(uint32_t cpu_id);

#endif

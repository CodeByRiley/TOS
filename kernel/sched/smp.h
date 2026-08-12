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

typedef void (*smp_work_fn)(void *arg);

void smp_boot_aps(void);

/* Queue an explicitly SMP-safe kernel job for any online AP. The function
 * runs concurrently with the BSP and therefore must synchronize shared data. */
int smp_submit_work(smp_work_fn fn, void *arg);
int smp_worker_count(void);
uint64_t smp_completed_work(void);

/* called from ap_trampoline after entering long mode. */
void ap_main(uint32_t cpu_id);

#endif

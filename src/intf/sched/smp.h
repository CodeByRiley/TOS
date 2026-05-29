#ifndef SMP_H
#define SMP_H

#include <stdint.h>

/* SMP bring-up. Walks the ACPI-enumerated CPUs and sends INIT-SIPI-SIPI to
 * each AP. Each AP runs ap_main() once it reaches 64-bit long mode. */

void smp_boot_aps(void);
void ap_main(uint32_t cpu_id);   /* C entry called by ap_trampoline.asm */

#endif

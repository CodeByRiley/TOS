/* kernel/devices/pit.h — 8254 PIT timer surface.
 *
 * The PIT drives the kernel tick counter at the frequency passed to
 * pit_init(). `pit_ticks()` is monotonically non-decreasing and consumed
 * by sleep_ticks, scheduler accounting, and userspace get_ticks().
 *
 * Implementation: kernel/devices/pit.c.
 */
#ifndef PIT_H
#define PIT_H

#include <stdint.h>

/* Program the PIT to fire IRQ0 at `freq_hz`. */
void     pit_init(uint32_t freq_hz);

/* Read the current tick counter. */
uint64_t pit_ticks(void);

#endif

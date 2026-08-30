/* kernel/devices/pit.h , 8254 PIT timer surface.
 *
 * The PIT drives the kernel tick counter at the frequency passed to
 * pit_init(). `pit_ticks()` is monotonically non-decreasing and consumed
 * by sleep_ticks, scheduler accounting, and userspace get_ticks().
 *
 * Implementation: kernel/devices/pit.c.
 */
#ifndef PIT_H
#define PIT_H

#include <utilities/types.h>
#include <stdint.h>

/* Program the PIT to fire IRQ0 at `freq_hz`. */
void     pit_init(u32 freq_hz);

/* Read the current tick counter. */
u64 pit_ticks(void);
u32 pit_get_freq(void);

/* Busy-wait on PIT channel 2. Independent of IRQ0 and of the tick counter,
 * so these are the only delays that work before interrupts are enabled. */
void     pit_delay_us(u64 us);
void     pit_delay_ms(u32 ms);

#endif

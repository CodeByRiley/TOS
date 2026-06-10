/* src/intf/sync/spinlock.h — test-and-set spinlock.
 *
 * Single-byte lock state on top of GCC atomic builtins. Busy-waits on
 * the cached copy with `pause` to play nice with the CPU's pipeline +
 * power management while spinning.
 *
 * Use spin_lock_irqsave / spin_unlock_irqrestore in any path that can
 * also run from an interrupt handler — otherwise an IRQ on the holding
 * CPU can recursively try to re-acquire the same lock and deadlock
 * against itself.
 */
#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>

typedef struct {
    volatile uint8_t locked;
} spinlock_t;

#define SPINLOCK_INIT { 0 }

static inline void spinlock_init(spinlock_t *l) { l->locked = 0; }

/* Acquire — busy-loop on the cached copy until it's clear, then xchg.
 * Acquire ordering pairs with spin_unlock's release. */
static inline void spin_lock(spinlock_t *l) {
    while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&l->locked, __ATOMIC_RELAXED)) {
            __asm__ volatile ("pause");
        }
    }
}

static inline void spin_unlock(spinlock_t *l) {
    __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
}

/* IRQ-safe variants. Save RFLAGS, disable interrupts, acquire. Restore
 * in reverse. The returned uint64_t is opaque — pass it straight to
 * spin_unlock_irqrestore. */
static inline uint64_t spin_lock_irqsave(spinlock_t *l) {
    uint64_t rflags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags) :: "memory");
    spin_lock(l);
    return rflags;
}

static inline void spin_unlock_irqrestore(spinlock_t *l, uint64_t rflags) {
    spin_unlock(l);
    if (rflags & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
}

#endif

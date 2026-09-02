/* kernel/sync/spinlock.h , test-and-set spinlock.
 *
 * Single-byte lock state on top of GCC atomic builtins. Busy-waits on
 * the cached copy with `pause` to play nice with the CPU's pipeline +
 * power management while spinning.
 *
 * Use spin_lock_irqsave / spin_unlock_irqrestore in any path that can
 * also run from an interrupt handler , otherwise an IRQ on the holding
 * CPU can recursively try to re-acquire the same lock and deadlock
 * against itself.
 */
#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>
#include <utilities/types.h>

struct spinlock {
  u8 locked; // volatile is not needed with gcc atomics
};

#define SPINLOCK_INIT {0}

SINLINE void spinlock_init(struct spinlock *l) { l->locked = 0; }

/* Acquire , busy-loop on the cached copy until it's clear, then xchg.
 * Acquire ordering pairs with spin_unlock's release. */
SINLINE void spin_lock(struct spinlock *l) {
  for (;;) {
    if (!__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE))
      return;

    while (__atomic_load_n(&l->locked, __ATOMIC_RELAXED))
      __asm__ volatile("pause");
  }
}

SINLINE void spin_unlock(struct spinlock *l) {
  __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
}

/* IRQ-safe variants. Save RFLAGS, disable interrupts, acquire. Restore
 * in reverse. The returned u64 is opaque , pass it straight to
 * spin_unlock_irqrestore. */
SINLINE u64 spin_lock_irqsave(struct spinlock *l) {
  u64 rflags;
  __asm__ volatile("pushfq; popq %0; cli" : "=r"(rflags)::"memory");
  spin_lock(l);
  return rflags;
}

SINLINE void spin_unlock_irqrestore(struct spinlock *l, u64 rflags) {
  spin_unlock(l);
  if (rflags & (1ULL << 9))
    __asm__ volatile("sti" ::: "memory");
}

#endif

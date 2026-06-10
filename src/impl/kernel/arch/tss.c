/* src/impl/kernel/arch/tss.c — placeholder TU.
 *
 * Intentionally empty. The TSS struct + per-CPU TSS table both live in
 * gdt.c (alongside the GDT descriptor install logic) because the two are
 * coupled — a TSS only matters once its GDT descriptor is loaded into TR.
 * This file is kept in the build graph for a future split if/when TSS
 * helpers grow beyond what fits in gdt.c.
 */

/* kernel/arch/offsets.c , source for the generated NASM offset include.
 *
 * Hand-written assembly reaches into three C structures by displacement:
 * SYSCALL entry finds its kernel stack through `gs:`, the ISR stubs test the
 * saved CS to decide whether they crossed a privilege boundary, and both
 * build frames the C side then parses as a struct. A literal in a .asm file
 * cannot be checked against the struct it is describing, so the literals are
 * generated from the structs instead.
 *
 * This translation unit is never linked. The build compiles it with -S and
 * tools/gen_asm_offsets.py lifts the @ASMDEF@ markers out of the assembler
 * output into build/generated/asm_offsets.inc, which every .asm file that
 * touches a shared layout includes. Reordering a field therefore changes the
 * assembly on the next build rather than silently desynchronising it.
 *
 * The C-side _Static_asserts in percpu.h / syscall.h / idt.h remain useful:
 * they document the intended layout and fail fast inside C. This file is what
 * proves the assembly agrees.
 */
#include <arch/gdt.h>
#include <arch/percpu.h>
#include <arch/syscall.h>
#include <interrupts/idt.h>

#include <stddef.h>

/* Emit `@ASMDEF@ <name> <value>` into the assembler output. The value arrives
 * through an "i" constraint so it must be a compile-time constant, and %c0
 * prints it without the AT&T immediate prefix. Nothing ever assembles this
 * text , the build stops at -S. */
#define ASM_DEFINE(name, value)                                                \
  __asm__ __volatile__("\n.ascii \"@ASMDEF@ " #name " %c0\""                   \
                       :                                                       \
                       : "i"((long)(value)))

void asm_offsets(void);

void asm_offsets(void) {
  /* struct cpu_local , reached through GS on every privilege entry. */
  ASM_DEFINE(CPU_LOCAL_SELF_OFF, offsetof(struct cpu_local, self));
  ASM_DEFINE(CPU_LOCAL_KERNEL_RSP_TOP_OFF,
             offsetof(struct cpu_local, kernel_rsp_top));
  ASM_DEFINE(CPU_LOCAL_USER_RSP_SAVE_OFF,
             offsetof(struct cpu_local, user_rsp_save));
  ASM_DEFINE(CPU_LOCAL_CURRENT_OFF, offsetof(struct cpu_local, current));
  ASM_DEFINE(CPU_LOCAL_SIZE, sizeof(struct cpu_local));

  /* struct syscall_frame , 15 saved registers followed by a complete ring-3
   * iretq image. The tail offsets let the stub reason about the return frame
   * without recounting pushes. */
  ASM_DEFINE(SYSCALL_FRAME_RAX_OFF, offsetof(struct syscall_frame, rax));
  ASM_DEFINE(SYSCALL_FRAME_RIP_OFF, offsetof(struct syscall_frame, rip));
  ASM_DEFINE(SYSCALL_FRAME_CS_OFF, offsetof(struct syscall_frame, cs));
  ASM_DEFINE(SYSCALL_FRAME_RFLAGS_OFF, offsetof(struct syscall_frame, rflags));
  ASM_DEFINE(SYSCALL_FRAME_RSP_OFF, offsetof(struct syscall_frame, rsp));
  ASM_DEFINE(SYSCALL_FRAME_SS_OFF, offsetof(struct syscall_frame, ss));
  ASM_DEFINE(SYSCALL_FRAME_SIZE, sizeof(struct syscall_frame));

  /* struct interrupt_frame , built by isr_common. Offsets are from the base
   * of the register image, i.e. from RSP right after the last push. */
  ASM_DEFINE(INTERRUPT_FRAME_INT_NUM_OFF,
             offsetof(struct interrupt_frame, int_num));
  ASM_DEFINE(INTERRUPT_FRAME_ERR_CODE_OFF,
             offsetof(struct interrupt_frame, err_code));
  ASM_DEFINE(INTERRUPT_FRAME_RIP_OFF, offsetof(struct interrupt_frame, rip));
  ASM_DEFINE(INTERRUPT_FRAME_CS_OFF, offsetof(struct interrupt_frame, cs));
  ASM_DEFINE(INTERRUPT_FRAME_SIZE, sizeof(struct interrupt_frame));

  /* Same CS slot measured from the stub's exit RSP, which points at int_num
   * once the register pops are done. This is the displacement the return-path
   * privilege test uses. */
  ASM_DEFINE(INTERRUPT_EXIT_CS_OFF,
             offsetof(struct interrupt_frame, cs) -
                 offsetof(struct interrupt_frame, int_num));
  /* Bytes of vector + error code the stub must discard before iretq. */
  ASM_DEFINE(INTERRUPT_EXIT_VEC_ERR_BYTES,
             offsetof(struct interrupt_frame, rip) -
                 offsetof(struct interrupt_frame, int_num));
  /* And once more from the base of the CPU-pushed iretq image, which is where
   * RSP sits after those bytes are dropped. Lets the return-path privilege
   * test run with swapgs still adjacent to iretq. */
  ASM_DEFINE(INTERRUPT_IRETQ_CS_OFF, offsetof(struct interrupt_frame, cs) -
                                         offsetof(struct interrupt_frame, rip));

  /* GDT selectors. RPL 3 is folded in here so no .asm file open-codes an
   * or-3 on a selector value. */
  ASM_DEFINE(GDT_KERNEL_CODE_SEL, GDT_KERNEL_CODE);
  ASM_DEFINE(GDT_KERNEL_DATA_SEL, GDT_KERNEL_DATA);
  ASM_DEFINE(GDT_USER_CODE_RPL3, GDT_USER_CODE | 3);
  ASM_DEFINE(GDT_USER_DATA_RPL3, GDT_USER_DATA | 3);
}

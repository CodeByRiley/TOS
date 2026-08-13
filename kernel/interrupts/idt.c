/* kernel/interrupts/idt.c — IDT install + dispatch.
 *
 * Builds the IDT, points each vector at one of the asm stubs in
 * kernel/arch/x86_64/interrupts/isr.asm (which save GPRs + a vector number then
 * call the C dispatcher), and routes IRQs 0x20-0x2F to dynamically registered
 * handlers via irq_install. Spurious IRQ7 / IRQ15 are masked and just
 * acked.
 *
 * exception_recovery_try / clear is a tiny setjmp-style trap so code
 * that may touch a faulting page can recover gracefully (used by the
 * framebuffer probe path).
 */
#include <arch/cpu.h>
#include <devices/lapic.h>
#include <devices/serial.h>
#include <interrupts/idt.h>
#include <interrupts/pic.h>
#include <memory/hhdm.h>
#include <memory/pmm.h>
#include <memory/vma.h>
#include <memory/vmm.h>
#include <sched/sched.h>
#include <stdint.h>
#include <utilities/log.h>
#include <utilities/panic.h>
#include <utilities/string.h>

#define MAX_IDT_ENTRIES 256
#define MAX_IRQ_HANDLERS 256

static struct idt_entry idt[MAX_IDT_ENTRIES];
static struct idt_ptr idtr;

extern uint64_t *kernel_pml4;
extern uint64_t isr_stub_table[48];
extern void isr240(void);
extern void isr255(void);

#define VEC_SMP_WORK_IPI 240
#define VEC_LAPIC_SPURIOUS 255

/* IST index 1 is the double-fault stack installed by gdt_install_tss. Any
 * other vector uses ist=0, meaning "keep the current stack". */
#define IST_DOUBLE_FAULT 1
#define VEC_DOUBLE_FAULT 8

static void idt_set_ist(int vec, uint64_t handler, uint8_t ist) {
  idt[vec].offset_low = handler & 0xFFFF;
  idt[vec].selector = 0x08;
  idt[vec].ist = ist;
  idt[vec].type_attr = 0x8E;
  idt[vec].offset_mid = (handler >> 16) & 0xFFFF;
  idt[vec].offset_high = (handler >> 32) & 0xFFFFFFFF;
  idt[vec].zero = 0;
}

static void idt_set(int vec, uint64_t handler) { idt_set_ist(vec, handler, 0); }

void idt_init(void) {
  for (int i = 0; i < 48; i++) {
    idt_set(i, isr_stub_table[i]);
  }
  idt_set(VEC_SMP_WORK_IPI, (uint64_t)isr240);
  idt_set(VEC_LAPIC_SPURIOUS, (uint64_t)isr255);
  /* #DF is the last chance to say anything before a triple fault resets
   * the machine. Give it a stack that cannot itself be the problem —
   * a stack overflow or a bad rsp0 otherwise reboots with no output. */
  idt_set_ist(VEC_DOUBLE_FAULT, isr_stub_table[VEC_DOUBLE_FAULT],
              IST_DOUBLE_FAULT);
  idtr.limit = sizeof(idt) - 1;
  idtr.base = (uint64_t)&idt;
  __asm__ volatile("lidt %0" : : "m"(idtr));
}

void idt_load_this_cpu(void) {
  /* Same shared IDT — IDTR is per-CPU, so each AP must lidt for itself. */
  __asm__ volatile("lidt %0" : : "m"(idtr));
}

typedef void (*irq_fn)(void);
static irq_fn irq_handlers[MAX_IRQ_HANDLERS] = {0};

void irq_install(uint8_t irq, irq_fn fn) { irq_handlers[irq] = fn; }

struct exception_recovery_state {
  uint8_t armed;
  uint8_t faulted;
  uint64_t resume_rip;
  uint64_t int_num;
  uint64_t err_code;
  uint64_t rip;
};

static struct exception_recovery_state exception_recovery = {0};

static void exception_recovery_arm(void *resume_ip) {
  exception_recovery.armed = 1;
  exception_recovery.faulted = 0;
  exception_recovery.resume_rip = (uint64_t)resume_ip;
  exception_recovery.int_num = 0;
  exception_recovery.err_code = 0;
  exception_recovery.rip = 0;
}

int exception_recovery_try(void) {
  exception_recovery_arm(__builtin_return_address(0));
  return 0;
}

void exception_recovery_clear(void) {
  exception_recovery.armed = 0;
  exception_recovery.faulted = 0;
  exception_recovery.resume_rip = 0;
  exception_recovery.int_num = 0;
  exception_recovery.err_code = 0;
  exception_recovery.rip = 0;
}

int exception_recovery_faulted(void) { return exception_recovery.faulted; }

uint64_t exception_recovery_int_num(void) { return exception_recovery.int_num; }

uint64_t exception_recovery_err_code(void) {
  return exception_recovery.err_code;
}

uint64_t exception_recovery_rip(void) { return exception_recovery.rip; }

static const char *exception_names[32] = {
    "divide error",
    "debug",
    "NMI",
    "breakpoint",
    "overflow",
    "bound range",
    "invalid opcode",
    "device not available",
    "double fault",
    "reserved",
    "invalid TSS",
    "segment not present",
    "stack-segment fault",
    "general protection",
    "page fault",
    "reserved",
    "x87 FPE",
    "alignment check",
    "machine check",
    "SIMD FPE",
    "virtualization",
    "control protection",
    "reserved",
    "reserved",
    "reserved",
    "reserved",
    "reserved",
    "reserved",
    "reserved",
    "reserved",
    "reserved",
    "reserved",
};

/* Page-table bits, for the fault reporter's manual walk. Deliberately its own
 * set rather than vmm.h's VMM_*: this code runs inside the fault handler and
 * must stay readable as a decoder of whatever the CPU actually found, not as
 * the mapper's view of what should be there. Values are the same layout —
 * see the table in vmm.h. */
#define PF_PAGE_SIZE 4096ULL
#define PF_ADDR_MASK VMM_ADDR_MASK
#define PF_PRESENT (1ULL << 0)
#define PF_WRITE (1ULL << 1)
#define PF_USER (1ULL << 2)
#define PF_HUGE (1ULL << 7)

static void fault_hex(const char *label, uint64_t value) {
  serial_write_str("[KERNEL]:   ");
  serial_write_str(label);
  serial_write_str("=");
  serial_write_hex(value);
  serial_write_str("\n");
}

static void fault_hex2(const char *left_label, uint64_t left,
                       const char *right_label, uint64_t right) {
  serial_write_str("[KERNEL]:   ");
  serial_write_str(left_label);
  serial_write_str("=");
  serial_write_hex(left);
  serial_write_str("  ");
  serial_write_str(right_label);
  serial_write_str("=");
  serial_write_hex(right);
  serial_write_str("\n");
}

static void fault_text(const char *label, const char *value) {
  serial_write_str("[KERNEL]:   ");
  serial_write_str(label);
  serial_write_str("=");
  serial_write_str(value);
  serial_write_str("\n");
}

static void fault_bit(const char *name, uint64_t error, uint8_t bit) {
  serial_write_str(name);
  serial_write_str("=");
  serial_write_char((error & (1ULL << bit)) ? '1' : '0');
}

static int canonical_address(uint64_t address) {
  uint64_t upper = address >> 48;
  return (address & (1ULL << 47)) ? upper == 0xFFFF : upper == 0;
}

static const char *fault_address_region(uint64_t address) {
  if (!canonical_address(address))
    return "non-canonical";
  if (address < 0x1000)
    return "null page";
  if (address <= 0x00007FFFFFFFFFFFULL)
    return "user canonical range";
  if (address < 0xFFFFE00000000000ULL)
    return "kernel direct-map/heap range";
  if (address < 0xFFFFFFFF80000000ULL)
    return "kernel MMIO range";
  return "kernel image range";
}

static uint64_t interrupted_rsp(const struct interrupt_frame *r) {
  if ((r->cs & 3) == 3)
    return r->rsp;

  /* Same-privilege interrupts do not push SS:RSP. The address immediately
   * after the hardware RFLAGS slot is the interrupted kernel RSP. */
  return (uint64_t)(uintptr_t)&r->rsp;
}

static uint64_t *fault_table(uint64_t physical) {
  uint64_t physical_limit = pmm_total_frames() * PF_PAGE_SIZE;
  if ((physical & (PF_PAGE_SIZE - 1)) != 0 || physical_limit == 0)
    return 0;
  if (physical > physical_limit || PF_PAGE_SIZE > physical_limit - physical)
    return 0;
  return phys_to_virt(physical);
}

static int fault_walk_next(const char *level, uint64_t index, uint64_t entry,
                           uint64_t **next) {
  serial_write_str("[KERNEL]:   ");
  serial_write_str(level);
  serial_write_str("[");
  serial_write_hex(index);
  serial_write_str("]=");
  serial_write_hex(entry);
  serial_write_str("\n");

  if (!(entry & PF_PRESENT)) {
    serial_write_str("[KERNEL]:   walk stopped: ");
    serial_write_str(level);
    serial_write_str(" is not present\n");
    return -1;
  }

  uint64_t physical = entry & PF_ADDR_MASK;
  *next = fault_table(physical);
  if (!*next) {
    serial_write_str("[KERNEL]:   walk stopped: next table physical ");
    serial_write_hex(physical);
    serial_write_str(" is outside the mapped RAM span\n");
    return -1;
  }
  return 0;
}

static void page_fault_walk(uint64_t address, uint64_t cr3) {
  uint64_t index[4] = {
      (address >> 39) & 0x1FF,
      (address >> 30) & 0x1FF,
      (address >> 21) & 0x1FF,
      (address >> 12) & 0x1FF,
  };

  fault_hex2("PML4 index", index[0], "PDPT index", index[1]);
  fault_hex2("PD index", index[2], "PT index", index[3]);

  uint64_t *table = fault_table(cr3 & PF_ADDR_MASK);
  if (!table) {
    serial_write_str(
        "[KERNEL]:   walk stopped: CR3 root is outside mapped RAM\n");
    return;
  }

  uint64_t entry = table[index[0]];
  if (fault_walk_next("PML4E", index[0], entry, &table) != 0)
    return;

  entry = table[index[1]];
  serial_write_str("[KERNEL]:   PDPTE[");
  serial_write_hex(index[1]);
  serial_write_str("]=");
  serial_write_hex(entry);
  serial_write_str("\n");
  if (!(entry & PF_PRESENT)) {
    serial_write_str("[KERNEL]:   walk stopped: PDPTE is not present\n");
    return;
  }
  if (entry & PF_HUGE) {
    uint64_t physical =
        (entry & 0x000FFFFFC0000000ULL) | (address & 0x3FFFFFFFULL);
    fault_hex("resolved 1 GiB physical", physical);
    return;
  }
  table = fault_table(entry & PF_ADDR_MASK);
  if (!table) {
    serial_write_str(
        "[KERNEL]:   walk stopped: PD physical is outside mapped RAM\n");
    return;
  }

  entry = table[index[2]];
  serial_write_str("[KERNEL]:   PDE[");
  serial_write_hex(index[2]);
  serial_write_str("]=");
  serial_write_hex(entry);
  serial_write_str("\n");
  if (!(entry & PF_PRESENT)) {
    serial_write_str("[KERNEL]:   walk stopped: PDE is not present\n");
    return;
  }
  if (entry & PF_HUGE) {
    uint64_t physical =
        (entry & 0x000FFFFFFFE00000ULL) | (address & 0x1FFFFFULL);
    fault_hex("resolved 2 MiB physical", physical);
    return;
  }
  table = fault_table(entry & PF_ADDR_MASK);
  if (!table) {
    serial_write_str(
        "[KERNEL]:   walk stopped: PT physical is outside mapped RAM\n");
    return;
  }

  entry = table[index[3]];
  serial_write_str("[KERNEL]:   PTE[");
  serial_write_hex(index[3]);
  serial_write_str("]=");
  serial_write_hex(entry);
  serial_write_str("\n");
  if (!(entry & PF_PRESENT)) {
    serial_write_str("[KERNEL]:   walk stopped: PTE is not present\n");
    return;
  }
  fault_hex("resolved physical", (entry & PF_ADDR_MASK) | (address & 0xFFF));
}

static void page_fault_report(const struct interrupt_frame *r,
                              uint64_t address) {
  uint64_t error = r->err_code;
  uint64_t cr0;
  uint64_t cr4;
  __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
  __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
  uint64_t cr3 = read_cr3();
  uint64_t rsp = interrupted_rsp(r);

  serial_write_str("[KERNEL]: page-fault details\n");
  fault_hex("address", address);
  fault_text("region", fault_address_region(address));
  fault_text("access", (error & (1ULL << 4))   ? "instruction fetch"
                       : (error & (1ULL << 1)) ? "write"
                                               : "read");
  fault_text("origin", (error & (1ULL << 2)) ? "user" : "supervisor");
  const char *cause = (error & (1ULL << 3))    ? "reserved page-table bit"
                      : (error & (1ULL << 5))  ? "protection key"
                      : (error & (1ULL << 6))  ? "shadow stack"
                      : (error & (1ULL << 15)) ? "SGX access-control"
                      : (error & 1)            ? "protection violation"
                                               : "not present";
  fault_text("cause", cause);

  serial_write_str("[KERNEL]:   error bits: ");
  fault_bit("P", error, 0);
  serial_write_str(" ");
  fault_bit("W", error, 1);
  serial_write_str(" ");
  fault_bit("U", error, 2);
  serial_write_str(" ");
  fault_bit("RSVD", error, 3);
  serial_write_str(" ");
  fault_bit("I/D", error, 4);
  serial_write_str(" ");
  fault_bit("PK", error, 5);
  serial_write_str(" ");
  fault_bit("SS", error, 6);
  serial_write_str(" ");
  fault_bit("SGX", error, 15);
  serial_write_str("\n");

  fault_hex2("cr0", cr0, "cr3", cr3);
  fault_hex2("cr4", cr4, "rflags", r->rflags);
  fault_hex2("cs", r->cs, "rsp", rsp);
  if ((r->cs & 3) == 3)
    fault_hex("ss", r->ss);

  struct task *task = task_current();
  if (task) {
    fault_hex2("task pid", (uint64_t)(uint32_t)task->pid, "task cr3",
               task->cr3);
    fault_text("task name", task->name[0] ? task->name : "unnamed");
    if (task->kstack) {
      uint64_t stack_base = (uint64_t)(uintptr_t)task->kstack;
      fault_hex2("kstack base", stack_base, "kstack top",
                 task->syscall_kstack_top);
    }
  } else {
    fault_text("task", "scheduler not initialised");
  }

  fault_hex2("rax", r->rax, "rbx", r->rbx);
  fault_hex2("rcx", r->rcx, "rdx", r->rdx);
  fault_hex2("rsi", r->rsi, "rdi", r->rdi);
  fault_hex2("rbp", r->rbp, "r8", r->r8);
  fault_hex2("r9", r->r9, "r10", r->r10);
  fault_hex2("r11", r->r11, "r12", r->r12);
  fault_hex2("r13", r->r13, "r14", r->r14);
  fault_hex("r15", r->r15);

  extern char _kernel_start[];
  extern char _kernel_end[];
  uint64_t kernel_start = (uint64_t)(uintptr_t)_kernel_start;
  uint64_t kernel_end = (uint64_t)(uintptr_t)_kernel_end;
  if (r->rip >= kernel_start && r->rip < kernel_end)
    fault_hex("kernel RIP offset", r->rip - kernel_start);

  page_fault_walk(address, cr3);
}

void isr_handler(struct interrupt_frame *r) {
  if (r->int_num < 32) {
    const char *name = "unknown";

    if (r->int_num < 32) {
      name = exception_names[r->int_num];
    }

    if (exception_recovery.armed) {
      log_write_exception(r->int_num, name, r->err_code, r->rip);
      exception_recovery.armed = 0;
      exception_recovery.faulted = 1;
      exception_recovery.int_num = r->int_num;
      exception_recovery.err_code = r->err_code;
      exception_recovery.rip = r->rip;
      r->rip = exception_recovery.resume_rip;
      r->rax = 1;
      return;
    }

    uint64_t cr2 = 0;
    if (r->int_num == 14) {
      __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

      /* KERNEL DEMAND PAGING */
      if (!(r->err_code & 0x1) && cr2 >= VMA_KERNEL_START) {
        uint64_t page_addr = cr2 & ~0xFFFULL;
        uint64_t phys = pmm_alloc_frame();
        if (phys) {
          memset((void *)phys_to_virt(phys), 0, 4096);
          if (vmm_map_in(kernel_pml4, page_addr, phys,
                         VMM_PRESENT | VMM_WRITE | VMM_NX) == 0)
            return;
          pmm_free_frame(phys);
        }
      }

      /* USER SPACE DEMAND PAGING */
      if (!(r->err_code & 0x1)) {
        struct task *t = task_current();
        if (t && t->user_pml4) {
          for (int i = 0; i < MAX_USER_VMAS; i++) {
            if (t->vmas[i].used && cr2 >= t->vmas[i].start &&
                cr2 < t->vmas[i].end) {
              uint64_t page_addr = cr2 & ~0xFFFULL;
              uint64_t phys = pmm_alloc_frame();

              if (phys) {
                memset((void *)phys_to_virt(phys), 0, 4096);
                if (vmm_map_in(t->user_pml4, page_addr, phys,
                               t->vmas[i].pte_flags) == 0)
                  return;
                pmm_free_frame(phys);
              }
              break;
            }
          }
        }
      }

      // Print the diagnostic report for the fault
      page_fault_report(r, cr2);
    }

    // If we get here, it's a fatal fault.
    // Check if the fault happened in User Mode (Ring 3)
    if ((r->cs & 3) == 3) {
      // User program misbehaved (e.g., null pointer). Kill it!
      log_write("USER SEGFAULT: killing task", KERNEL, LOG_ERROR);
      task_exit(-11); // -11 is conventionally SIGSEGV
      //panic_from_exception(name, r, cr2, r->int_num == 14);
    } else {
      // Kernel misbehaved. The system is compromised. Panic!
      panic_from_exception(name, r, cr2, r->int_num == 14);
    }
    // If we get here, it's a fatal fault.
    // Later, you can replace this panic with a task_kill() to kill the process!
    // panic_from_exception(name, r, cr2, r->int_num == 14);
  } else if (r->int_num < 48) {
    uint8_t irq = r->int_num - 32;
    if (irq_handlers[irq])
      irq_handlers[irq]();
    pic_send_eoi(irq);
    /* EOI must precede a context switch or the PIC keeps IRQ0 in service and
     * no later timer tick can preempt the task we switch to. Restrict timer
     * preemption to interrupted ring-3 code: kernel subsystems are not built
     * for arbitrary re-entry while their globals are half-mutated. */
    if (irq == 0 && (r->cs & 3) == 3)
      sched_preempt_tick();
  } else if (r->int_num == VEC_SMP_WORK_IPI) {
    lapic_eoi();
  } else if (r->int_num == VEC_LAPIC_SPURIOUS) {
    /* Intel specifies that a spurious LAPIC interrupt must not be EOIed. */
  }
  // if (r->int_num < 32) {
  //     serial_write_str("\n!! exception ");
  //     serial_write_hex(r->int_num);
  //     serial_write_str(" (");
  //     serial_write_str(exception_names[r->int_num]);
  //     serial_write_str(")\n  err=");
  //     serial_write_hex(r->err_code);
  //     serial_write_str("\n  rip=");
  //     serial_write_hex(r->rip);
  //     serial_write_str("\n");
  //     for (;;) __asm__ volatile ("cli; hlt");
  // } else if (r->int_num < 48) {
  //     uint8_t irq = r->int_num - 32;
  //     if (irq_handlers[irq]) irq_handlers[irq]();
  //     pic_send_eoi(irq);
  // }
}

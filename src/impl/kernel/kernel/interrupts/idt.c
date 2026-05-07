#include "interrupts/idt.h"
#include "utilities/log.h"
#include "devices/serial.h"
#include "interrupts/pic.h"
#include <stdint.h>

#define MAX_IDT_ENTRIES 512
#define MAX_IRQ_HANDLERS 256

static struct idt_entry idt[MAX_IDT_ENTRIES];
static struct idt_ptr idtr;

extern uint64_t isr_stub_table[48];

static void idt_set(int vec, uint64_t handler) {
    idt[vec].offset_low  = handler & 0xFFFF;
    idt[vec].selector    = 0x08;
    idt[vec].ist         = 0;
    idt[vec].type_attr   = 0x8E;
    idt[vec].offset_mid  = (handler >> 16) & 0xFFFF;
    idt[vec].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[vec].zero        = 0;
}

void idt_init(void) {
    for (int i = 0; i < 48; i++) {
        idt_set(i, isr_stub_table[i]);
    }
    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uint64_t)&idt;
    __asm__ volatile ("lidt %0" : : "m"(idtr));
}

typedef void (*irq_fn)(void);
static irq_fn irq_handlers[MAX_IRQ_HANDLERS] = { 0 };

void irq_install(uint8_t irq, irq_fn fn) {
    irq_handlers[irq] = fn;
}

struct exception_recovery_state {
    uint8_t armed;
    uint8_t faulted;
    uint64_t resume_rip;
    uint64_t int_num;
    uint64_t err_code;
    uint64_t rip;
};

static struct exception_recovery_state exception_recovery = { 0 };

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

int exception_recovery_faulted(void) {
    return exception_recovery.faulted;
}

uint64_t exception_recovery_int_num(void) {
    return exception_recovery.int_num;
}

uint64_t exception_recovery_err_code(void) {
    return exception_recovery.err_code;
}

uint64_t exception_recovery_rip(void) {
    return exception_recovery.rip;
}

struct registers {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    uint64_t int_num;
    uint64_t err_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

static const char *exception_names[32] = {
    "divide error", "debug", "NMI", "breakpoint",
    "overflow", "bound range", "invalid opcode", "device not available",
    "double fault", "reserved", "invalid TSS", "segment not present",
    "stack-segment fault", "general protection", "page fault", "reserved",
    "x87 FPE", "alignment check", "machine check", "SIMD FPE",
    "virtualization", "control protection", "reserved", "reserved",
    "reserved", "reserved", "reserved", "reserved",
    "reserved", "reserved", "reserved", "reserved",
};

void isr_handler(struct registers *r) {
  if (r->int_num < 32) {
    const char *name = "unknown";

    if (r->int_num < 32) {
      name = exception_names[r->int_num];
    }

    log_write_exception(r->int_num, name, r->err_code, r->rip);
    if (exception_recovery.armed) {
      exception_recovery.armed = 0;
      exception_recovery.faulted = 1;
      exception_recovery.int_num = r->int_num;
      exception_recovery.err_code = r->err_code;
      exception_recovery.rip = r->rip;
      r->rip = exception_recovery.resume_rip;
      r->rax = 1;
      return;
    }

    if (r->int_num == 14) {
        uint64_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        log_write_hex("  cr2 (fault addr) =", cr2, KERNEL, LOG_INFO);
    }

    log_write("kernel panic: unhandled exception", KERNEL, LOG_FATAL);
    for (;;) __asm__ volatile ("cli; hlt");
  } else if (r->int_num < 48) {
    uint8_t irq = r->int_num - 32;
    if (irq_handlers[irq])
      irq_handlers[irq]();
    pic_send_eoi(irq);
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

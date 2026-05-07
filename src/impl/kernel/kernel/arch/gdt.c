#include "arch/gdt.h"
#include "utilities/log.h"
#include "utilities/string.h"
#include <stdint.h>

struct __attribute__((packed)) gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;     // limit[19:16] in low nibble, flags in high nibble
    uint8_t  base_high;
};

struct __attribute__((packed)) tss_desc {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
};

struct __attribute__((packed)) gdtr {
    uint16_t limit;
    uint64_t base;
};

struct __attribute__((packed)) tss {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1, ist2, ist3, ist4, ist5, ist6, ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
};

// 5 normal 8-byte entries + 1 16-byte TSS = 56 bytes
static uint8_t  gdt_table[56] __attribute__((aligned(8)));
static struct tss kernel_tss __attribute__((aligned(8)));
static struct gdtr gdtr;
static uint8_t  kernel_stack[16384] __attribute__((aligned(16)));

static void set_entry(uint16_t selector, uint8_t access, uint8_t flags) {
    struct gdt_entry *e = (struct gdt_entry*)&gdt_table[selector];
    e->limit_low   = 0xFFFF;
    e->base_low    = 0;
    e->base_mid    = 0;
    e->access      = access;
    e->granularity = 0x0F | (flags << 4);
    e->base_high   = 0;
}

static void set_tss(uint16_t selector, uint64_t base, uint32_t limit) {
    struct tss_desc *t = (struct tss_desc*)&gdt_table[selector];
    t->limit_low   = limit & 0xFFFF;
    t->base_low    = base & 0xFFFF;
    t->base_mid    = (base >> 16) & 0xFF;
    t->access      = 0x89;                  // available 64-bit TSS
    t->granularity = (limit >> 16) & 0x0F;
    t->base_high   = (base >> 24) & 0xFF;
    t->base_upper  = (uint32_t)(base >> 32);
    t->reserved    = 0;
}

void gdt_init(void) {
    memset(gdt_table, 0, sizeof(gdt_table));        // null at 0x00
    set_entry(GDT_KERNEL_CODE, 0x9A, 0xA);          // kernel code, 64-bit
    set_entry(GDT_KERNEL_DATA, 0x92, 0xC);          // kernel data
    set_entry(GDT_USER_DATA,   0xF2, 0xC);          // user data, DPL=3
    set_entry(GDT_USER_CODE,   0xFA, 0xA);          // user code, DPL=3, 64-bit

    memset(&kernel_tss, 0, sizeof(kernel_tss));
    kernel_tss.iomap_base = sizeof(kernel_tss);     // no I/O permission map
    kernel_tss.rsp0 = (uint64_t)(kernel_stack + sizeof(kernel_stack));

    set_tss(GDT_TSS, (uint64_t)&kernel_tss, sizeof(kernel_tss) - 1);

    gdtr.limit = sizeof(gdt_table) - 1;
    gdtr.base  = (uint64_t)gdt_table;

    __asm__ volatile (
        "lgdt %0                 \n"
        "pushq $0x08             \n"   // new CS = kernel code
        "leaq 1f(%%rip), %%rax   \n"
        "pushq %%rax             \n"
        "lretq                   \n"   // far return reloads CS
        "1:                      \n"
        "mov $0x10, %%ax         \n"   // kernel data
        "mov %%ax, %%ds          \n"
        "mov %%ax, %%es          \n"
        "mov %%ax, %%fs          \n"
        "mov %%ax, %%gs          \n"
        "mov %%ax, %%ss          \n"
        "mov $0x28, %%ax         \n"   // TSS selector
        "ltr %%ax                \n"
        :
        : "m"(gdtr)
        : "rax", "memory"
    );

    log_write("GDT + TSS loaded", KERNEL, LOG_INFO);
}

void tss_set_rsp0(uint64_t rsp0) {
    kernel_tss.rsp0 = rsp0;
}

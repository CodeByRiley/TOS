/* Kernel entry point and staged subsystem initialization. */
#include <acpi/acpi.h>
#include <arch/gdt.h>
#include <arch/percpu.h>
#include <arch/syscall.h>
#include <boot/multiboot2.h>
#include <devices/lapic.h>
#include <devices/pit.h>
#include <devices/serial.h>
#include <devices/usb.h>
#include <display/fonts/ttf.h>
#include <display/framebuffer.h>
#include <display/print.h>
#include <display/tty.h>
#include <drivers/driver.h>
#include <drivers/network/eth/e1000/e1000.h>
#include <drivers/sound/sb16.h>
#include <drivers/storage/ahci.h>
#include <drivers/video/nvidia/nvidia.h>
#include <drivers/video/virtio/virtio_gpu.h>
#include <fs/fat.h>
#include <fs/fat_ahci.h>
#include <input/keyboard.h>
#include <input/mouse.h>
#include <interrupts/idt.h>
#include <interrupts/pic.h>
#include <isa/isa.h>
#include <loader/process.h>
#include <memory/memory.h>
#include <msg/msg.h>
#include <pci/pci.h>
#include <sched/sched.h>
#include <sched/smp.h>
#include <stdint.h>
#include <utilities/log.h>

extern struct AHCI_DEVICE_DATA *g_ahci_dev;
extern uint64_t *kernel_pml4;

static void early_console_init(void) {
    print_clear();
    print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK);
    serial_init();
    log_write("serial initialised", KERNEL, LOG_INFO);
}

static void arch_init(void) {
	log_write("initialising gdt", KERNEL, LOG_INFO);
    gdt_init();
    idt_init();
    log_write("idt initialised", KERNEL, LOG_INFO);
    pic_remap();
    pit_init(500);
    log_write("pit initialised", KERNEL, LOG_INFO);
}

static void memory_init(uint64_t mb2_addr) {
    pmm_init(mb2_addr);
    vmm_init();
    vma_init();
    heap_init();
    log_write("pmm, vmm, heap initialised", KERNEL, LOG_INFO);

    /* Demand-paging boot check. */
    log_write("Testing Demand Paging...", KERNEL, LOG_INFO);
    uint64_t test_page = vma_alloc(4096);
    if (!test_page) {
        log_write("Demand Paging Test: vma_alloc failed!", KERNEL, LOG_ERROR);
        return;
    }
    log_write("Allocated unmapped VMA. Preparing to write to it...", KERNEL, LOG_INFO);
    uint32_t *ptr = (uint32_t *)test_page;
    *ptr = 0xDEADBEEF;
    if (*ptr == 0xDEADBEEF) {
        log_write("Demand Paging Success! Hardware fault handled dynamically.",
                  KERNEL, LOG_INFO);
    } else {
        log_write("Demand Paging FAILED!", KERNEL, LOG_ERROR);
    }
    vfree(test_page);
}

static void acpi_and_smp_init(uint64_t mb2_addr) {
    if (acpi_init(mb2_addr) == 0) {
        lapic_init(acpi_lapic_phys());
        percpu_init_bsp((uint8_t)lapic_id());
        percpu_set_count(acpi_cpu_count());
        gdt_load_tss_this_cpu(0);
    } else {
        log_write("ACPI: SMP unavailable, staying UP", KERNEL, LOG_INFO);
    }
}

static void ipc_and_sched_init(void) {
    msg_init();
    log_write("message queue initialised", KERNEL, LOG_INFO);
    sched_init();
    log_write("scheduler initialised", KERNEL, LOG_INFO);
}

static void display_init(uint64_t mb2_addr) {
    log_write("initialising framebuffer", KERNEL, LOG_INFO);
    framebuffer_init(mb2_addr);
    log_write("framebuffer initialised", KERNEL, LOG_INFO);

    if (!nvidia_display_active()) {
        if (nvidia_device_count() > 0) {
            log_write("display: NVIDIA detected but not driving scanout",
                      KERNEL, LOG_INFO);
        }
        if (virtio_gpu_init() == 0) {
            if (framebuffer_attach_virtio() != 0) {
                log_write("display: virtio attach failed, staying on MB2 fb",
                          KERNEL, LOG_WARN);
            } else {
                struct task *flush = task_spawn(framebuffer_flush_thread_entry);
                if (flush)
                    sched_set_priority(flush, SCHED_PRIO_HIGH);
                log_write("display: fb flush thread spawned", KERNEL, LOG_INFO);
            }
        } else {
            log_write("display: no virtio-gpu, staying on MB2 fb", KERNEL, LOG_INFO);
        }
    } else {
        log_write("display: NVIDIA driving scanout, keeping its framebuffer",
                  KERNEL, LOG_INFO);
    }
}

static void input_init(void) {
    keyboard_init();
    log_write("keyboard initialised", KERNEL, LOG_INFO);
    mouse_init();
    log_write("mouse initialised", KERNEL, LOG_INFO);
    mouse_set_bounds((int32_t)framebuffer_width(),
                     (int32_t)framebuffer_height());
}

static void devices_init(void) {
    driver_core_init();
    if (nvidia_driver_register() != 0) {
        log_write("nvidia: driver registration failed", KERNEL, LOG_ERROR);
    }
    pci_init();
    driver_probe_pci_devices();
    isa_probe_devices();
    sb16_driver_init();
    e1000_driver_init();
    usb_init();
    ahci_init();
}

static void filesystem_init(uint64_t mb2_addr) {
    struct MB2_TAG_MODULE *m = mb2_find_module(mb2_addr, "rootfs");
    if (!m) {
        log_write("rootfs: no rootfs module", KERNEL, LOG_ERROR);
        for (;;)
            __asm__ volatile("hlt");
    }

    bool fs_mounted = false;

    if (g_ahci_dev && fat_mount_from_ahci(g_ahci_dev, 0) == 0) {
        log_write("rootfs: mounted from AHCI SATA drive", FILESYS, LOG_INFO);
        fs_mounted = true;
    } else {
        log_write("rootfs: AHCI unavailable or unformatted, trying ramdisk...",
                  KERNEL, LOG_WARN);
    }

    if (!fs_mounted && m) {
        if (fat_init(phys_to_virt(m->mod_start),
                     m->mod_end - m->mod_start) == 0) {
            log_write("rootfs: mounted from Multiboot2 ramdisk module",
                      FILESYS, LOG_INFO);
            fs_mounted = true;
        }
    }

    if (!fs_mounted) {
        log_write("PANIC: Unable to mount any root filesystem!", KERNEL, LOG_ERROR);
        for (;;)
            __asm__ volatile("hlt");
    }
}

static void late_init(void) {
    nvidia_driver_late_init();
    ttf_init_font();
    if (g_sys_font != NULL) {
        tty_resize();
        static const char hello[] = "Hello from TTF!\n";
        tty_write(hello, sizeof(hello) - 1);
    }

    tty_init();
    log_write("tty: fallback text console ready", KERNEL, LOG_INFO);
    task_spawn(tty_thread_entry);
    log_write("tty: render thread spawned", KERNEL, LOG_INFO);

    static uint8_t syscall_kstack[16384] ALIGNED(16);
    syscall_init((uint64_t)(syscall_kstack + sizeof(syscall_kstack)));
}

static void enable_interrupts_and_smp(void) {
    pic_clear_mask(0);
    pic_clear_mask(1);
    pic_clear_mask(2);   /* cascade */
    pic_clear_mask(12);  /* mouse */
    __asm__ volatile("sti");

    smp_boot_aps();
    log_write("kernel booted", KERNEL, LOG_INFO);
}

static void userspace_init(void) {
    char *winman_argv[] = {(char *)"winman", 0};
    long winman_pid = process_spawn_async("/system/bin/winman.elf", winman_argv);
    if (winman_pid < 0) {
        log_write("winman: launch failed — TTY-only mode", USER, LOG_INFO);
    } else {
        log_write_hex("winman: spawn returned pid =", (uint64_t)winman_pid,
                      USER, LOG_INFO);
    }

    char *sh_argv[] = {(char *)"sh", 0};
    long code = process_exec("/system/bin/sh.elf", sh_argv);
    log_write_hex("shell exited code =", code, USER, LOG_INFO);
}


void kernel_main(uint64_t mb2_addr) {
  early_console_init();
  arch_init();
  memory_init(mb2_addr);
  acpi_and_smp_init(mb2_addr);
  ipc_and_sched_init();
  devices_init(); /* PCI + drivers first so display can see them */
  display_init(mb2_addr);
  input_init(); /* after real framebuffer exists */
  filesystem_init(mb2_addr);
  late_init();
  enable_interrupts_and_smp();
  userspace_init();

  /* BSP becomes idle */
  for (;;)
    __asm__ volatile("hlt");
}

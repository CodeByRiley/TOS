#include "memory/heap.h"
#include "memory/vma.h"
#include <drivers/driver.h>
#include <drivers/storage/ahci.h>
#include <memory/hhdm.h>
#include <memory/pmm.h>
#include <memory/vmm.h>
#include <utilities/log.h>
#include <utilities/string.h>

/* AHCI Memory Register Offsets (from BAR5) */
#define AHCI_CAP 0x00 // Host Capability
#define AHCI_GHC 0x04 // Global Host Control
#define AHCI_PI 0x0C  // Ports Implemented
#define AHCI_VS 0x10  // Version

#define AHCI_GHC_AE (1u << 31) // AHCI Enable
#define AHCI_GHC_HR (1u << 0)  // HBA Reset

#define AHCI_CAP_NCS_SHIFT 8   // Number of Command Slots, minus one
#define AHCI_CAP_NCS_MASK 0x1F

/* Every wait in this driver is a bounded spin: the probe runs before the
 * scheduler exists, so there is no sleep to yield to, and an unbounded loop
 * on a register the HBA never updates wedges the boot. */
#define AHCI_SPIN_LIMIT 5000000u

extern u64 *kernel_pml4;

struct AHCI_DEVICE_DATA *g_ahci_dev = NULL;

SINLINE void ahci_pause(void) { __asm__ volatile("pause"); }

static void ahci_write32(const volatile void *abar, u32 off,
                         u32 val) {
  *((volatile u32 *)((uintptr_t)abar + off)) = val;
}

static u32 ahci_read32(const volatile void *base, u32 offset) {
  return *(volatile u32 *)((uintptr_t)base + offset);
}

static int ahci_match(const struct device *dev) {
  if (dev->bus != DEVICE_BUS_PCI)
    return 0;

  // 0x01 = Mass Storage, 0x06 = SATA, 0x01 = AHCI
  return dev->bus_info.pci.class_code == 0x01 &&
         dev->bus_info.pci.subclass == 0x06 &&
         dev->bus_info.pci.prog_if == 0x01;
}

// Helper to calculate the virtual address of a specific port's registers
static volatile void *ahci_port_base(void *abar_virtual, int port) {
  return (volatile void *)((uintptr_t)abar_virtual + 0x100 + (port * 0x80));
}

// Helper to wait for a command slot to be free
static int ahci_find_cmd_slot(volatile void *port_base, u32 slot_count) {
  // PxSACT (SATA Active) and PxCI (Command Issue) tell us which slots are busy
  u32 slots = ahci_read32(port_base, AHCI_PORT_PxCI) |
                   ahci_read32(port_base, AHCI_PORT_PxSACT);

  for (u32 i = 0; i < slot_count; i++) {
    if (!(slots & (1u << i))) {
      return (int)i; // Found a free slot
    }
  }
  return -1; // All slots busy
}

/* Command table for `slot`, inside the port's single-frame table array. */
static struct AHCI_CMD_TABLE *ahci_cmd_table(struct AHCI_PORT *p, int slot) {
  return (struct AHCI_CMD_TABLE *)((uintptr_t)p->cmd_table_virt +
                                   (u32)slot * AHCI_CMD_TABLE_STRIDE);
}

static u64 ahci_cmd_table_phys(struct AHCI_PORT *p, int slot) {
  return p->cmd_table_phys + (u64)(u32)slot * AHCI_CMD_TABLE_STRIDE;
}

/* Clear ST and FRE, then wait for the HBA to acknowledge by dropping CR and
 * FR. Touching PxCLB/PxFB while either is still running is undefined. */
static int ahci_port_stop(volatile void *port_base) {
  u32 cmd = ahci_read32(port_base, AHCI_PORT_PxCMD);
  cmd &= ~(AHCI_PxCMD_ST | AHCI_PxCMD_FRE);
  ahci_write32(port_base, AHCI_PORT_PxCMD, cmd);

  for (u32 i = 0; i < AHCI_SPIN_LIMIT; i++) {
    if (!(ahci_read32(port_base, AHCI_PORT_PxCMD) &
          (AHCI_PxCMD_CR | AHCI_PxCMD_FR)))
      return 0;
    ahci_pause();
  }
  return -1;
}

/* FRE has to be up before ST, and ST must not be set while CR is still
 * running from a previous engine start. */
static int ahci_port_start(volatile void *port_base) {
  u32 i;
  for (i = 0; i < AHCI_SPIN_LIMIT; i++) {
    if (!(ahci_read32(port_base, AHCI_PORT_PxCMD) & AHCI_PxCMD_CR))
      break;
    ahci_pause();
  }
  if (i == AHCI_SPIN_LIMIT)
    return -1;

  u32 cmd = ahci_read32(port_base, AHCI_PORT_PxCMD);
  ahci_write32(port_base, AHCI_PORT_PxCMD, cmd | AHCI_PxCMD_FRE);

  cmd = ahci_read32(port_base, AHCI_PORT_PxCMD);
  ahci_write32(port_base, AHCI_PORT_PxCMD, cmd | AHCI_PxCMD_ST);
  return 0;
}

/* The drive must have BSY and DRQ clear before a new command FIS is built. */
static int ahci_wait_ready(volatile void *port_base) {
  for (u32 i = 0; i < AHCI_SPIN_LIMIT; i++) {
    u32 tfd = ahci_read32(port_base, AHCI_PORT_PxTFD);
    if (!(tfd & (AHCI_PxTFD_STS_BSY | AHCI_PxTFD_STS_DRQ)))
      return 0;
    ahci_pause();
  }
  return -1;
}

/* PxIS and PxSERR are write-1-to-clear; hand each register its own read
 * value back so stale bits do not confuse the next command's error check. */
static void ahci_clear_status(volatile void *port_base) {
  ahci_write32(port_base, AHCI_PORT_PxIS,
               ahci_read32(port_base, AHCI_PORT_PxIS));
  ahci_write32(port_base, AHCI_PORT_PxSERR,
               ahci_read32(port_base, AHCI_PORT_PxSERR));
}

/* One command builder for reads, writes, IDENTIFY and cache barriers.
 * A failed/timed-out command poisons this port: never reuse its DMA memory
 * while the HBA may still own it. Stop the engine before returning. */
static int ahci_command(struct AHCI_DEVICE_DATA *dev, int port, u8 command,
                         u64 lba, u32 count, void *buffer, u32 bytes, int writing) {
    if (!dev || port < 0 || port >= AHCI_MAX_PORTS ||
        bytes > 4u * 1024u * 1024u || (bytes && !buffer)) return -1;
    u64 physical = (u64)(uintptr_t)buffer;
    if (bytes && ((physical & 1) || physical > UINT64_MAX - bytes)) return -1;
    if (bytes && !(ahci_read32(dev->abar_virtual, AHCI_CAP) & (1u << 31)) &&
        physical + bytes > (1ull << 32)) return -1;
    struct AHCI_PORT *p = &dev->ports[port];
    spin_lock(&p->io_lock);
    int result = -1;
    if (!p->is_active || p->signature != AHCI_SIG_SATA) goto done;
    volatile void *base = ahci_port_base(dev->abar_virtual, port);
    if (ahci_wait_ready(base)) goto failed;
    ahci_clear_status(base);
    int slot = ahci_find_cmd_slot(base, p->slots);
    if (slot < 0) goto done;
    struct AHCI_CMD_HEADER *header = &((struct AHCI_CMD_HEADER *)p->cmd_list_virt)[slot];
    struct AHCI_CMD_TABLE *table = ahci_cmd_table(p, slot);
    memset(header, 0, sizeof(*header));
    memset(table, 0, sizeof(*table));
    u64 table_physical = ahci_cmd_table_phys(p, slot);
    header->cfl = sizeof(struct FIS_REG_H2D) / sizeof(u32);
    header->write = writing;
    header->prdt_length = bytes ? 1 : 0;
    header->command_table_base = (u32)table_physical;
    header->command_table_base_upper = (u32)(table_physical >> 32);
    if (bytes) {
        table->prdt[0].data_base = (u32)(uintptr_t)buffer;
        table->prdt[0].data_base_upper = (u32)((u64)(uintptr_t)buffer >> 32);
        table->prdt[0].size = (bytes - 1) | (1u << 31);
    }
    struct FIS_REG_H2D *fis = &table->command_fis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->command = command;
    fis->device = 1u << 6;
    fis->lba0 = (u8)lba; fis->lba1 = (u8)(lba >> 8); fis->lba2 = (u8)(lba >> 16);
    fis->lba3 = (u8)(lba >> 24); fis->lba4 = (u8)(lba >> 32); fis->lba5 = (u8)(lba >> 40);
    fis->countl = (u8)count; fis->counth = (u8)(count >> 8);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    ahci_write32(base, AHCI_PORT_PxCI, 1u << slot);
    for (u32 spins = 0; spins < AHCI_SPIN_LIMIT; spins++) {
        u32 active = ahci_read32(base, AHCI_PORT_PxCI);
        if (ahci_read32(base, AHCI_PORT_PxIS) & AHCI_PxIS_TFES) goto failed;
        if (!(active & (1u << slot))) {
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if ((ahci_read32(base, AHCI_PORT_PxTFD) & AHCI_PxTFD_STS_ERR) ||
                header->prdbc != bytes) goto failed;
            result = 0;
            goto done;
        }
        ahci_pause();
    }
failed:
    p->is_active = 0;
    if (ahci_port_stop(base)) {
        /* An unquiesced DMA engine cannot safely return to a caller that will
         * free the buffer. Fail-stop rather than permit arbitrary corruption. */
        log_write("AHCI: cannot stop failed DMA engine", KERNEL, LOG_ERROR);
        for (;;) __asm__ volatile("cli; hlt");
    }
done:
    spin_unlock(&p->io_lock);
    return result;
}

static int sector_command(struct AHCI_DEVICE_DATA *dev, int port, u64 lba,
                           u32 count, void *buffer, int writing) {
    if (!count || count > 8192 || lba >= (1ull << 48) ||
        count > (1ull << 48) - lba) return -1;
    return ahci_command(dev, port, writing ? 0x35 : 0x25,
                         lba, count, buffer, count * 512u, writing);
}
int ahci_read_sector(struct AHCI_DEVICE_DATA *dev, int port, u64 lba,
                     u32 count, void *buffer) {
    return sector_command(dev, port, lba, count, buffer, 0);
}
int ahci_write_sector(struct AHCI_DEVICE_DATA *dev, int port, u64 lba,
                      u32 count, void *buffer) {
    return sector_command(dev, port, lba, count, buffer, 1);
}
int ahci_identify(struct AHCI_DEVICE_DATA *dev, int port, void *buffer) {
    return ahci_command(dev, port, 0xec, 0, 0, buffer, 512, 0);
}
int ahci_flush_cache(struct AHCI_DEVICE_DATA *dev, int port) {
    return ahci_command(dev, port, 0xea, 0, 0, 0, 0, 0);
}

static int ahci_init_port(struct AHCI_DEVICE_DATA *dev, int port) {
    void *abar_virtual = dev->abar_virtual;
    volatile void *port_base = ahci_port_base(abar_virtual, port);
    struct AHCI_PORT *p = &dev->ports[port];

    u32 ssts = ahci_read32(port_base, AHCI_PORT_PxSSTS);
    if ((ssts & AHCI_PxSSTS_DET_MASK) != AHCI_PxSSTS_DET_PRESENT) {
        return -1; // No drive plugged in
    }

    p->signature = ahci_read32(port_base, AHCI_PORT_PxSIG);
    log_write_hex("AHCI: Drive found on port", port, KERNEL, LOG_INFO);
    log_write_hex("AHCI: Signature =", p->signature, KERNEL, LOG_INFO);

    // Stop the port engine and wait for CR and FR to clear
    if (ahci_port_stop(port_base) != 0) {
        log_write("AHCI: timed out stopping port engine", KERNEL, LOG_ERROR);
        return -1;
    }

    /* Every address below is handed to the HBA for DMA, so it has to be a
     * real physical address. The kernel heap lives at its own VA base, not
     * in the HHDM, so virt_to_phys() is meaningless on a kmalloc pointer ,
     * allocate raw frames and address them through the HHDM instead.
     *
     * A frame base is 4 KiB aligned, which satisfies the 1 KiB command-list
     * and 256-byte received-FIS alignment rules at offsets 0 and 1024. */
    u64 clb_frame = pmm_alloc_frame_below(1ull << 32);
    if (!clb_frame) {
        log_write("AHCI: no frame for command list", KERNEL, LOG_ERROR);
        return -1;
    }
    p->cmd_list_phys = clb_frame;
    p->cmd_list_virt = phys_to_virt(clb_frame);
    memset(p->cmd_list_virt, 0, 4096);

    p->fis_phys = clb_frame + 1024;
    p->fis_virt = (void *)((uintptr_t)p->cmd_list_virt + 1024);
    /* One frame holds AHCI_SLOTS_PER_PORT command tables at a 256-byte
     * stride, each therefore 128-byte aligned as the spec requires. */
    u64 ctba_frame = pmm_alloc_frame_below(1ull << 32);
    if (!ctba_frame) {
        log_write("AHCI: no frame for command tables", KERNEL, LOG_ERROR);
        pmm_free_frame(clb_frame);
        return -1;
    }
    p->cmd_table_phys = ctba_frame;
    p->cmd_table_virt = phys_to_virt(ctba_frame);
    memset(p->cmd_table_virt, 0, 4096);
    /* CAP.NCS holds the slot count minus one. */
    u32 ncs = ((ahci_read32(abar_virtual, AHCI_CAP) >> AHCI_CAP_NCS_SHIFT) &
                    AHCI_CAP_NCS_MASK) + 1;
    p->slots = ncs < AHCI_SLOTS_PER_PORT ? ncs : AHCI_SLOTS_PER_PORT;

    ahci_write32(port_base, AHCI_PORT_PxCLB, (u32)p->cmd_list_phys);
    ahci_write32(port_base, AHCI_PORT_PxCLBU, (u32)(p->cmd_list_phys >> 32));
    ahci_write32(port_base, AHCI_PORT_PxFB, (u32)p->fis_phys);
    ahci_write32(port_base, AHCI_PORT_PxFBU, (u32)(p->fis_phys >> 32));
    /* Clear anything the firmware left behind before starting the engine. */
    ahci_clear_status(port_base);

    // Restart the port engine
    if (ahci_port_start(port_base) != 0) {
        log_write("AHCI: timed out restarting port engine", KERNEL, LOG_ERROR);
        pmm_free_frame(ctba_frame);
        pmm_free_frame(clb_frame);
        return -1;
    }
    p->is_active = 1; // Mark port as fully initialized!

    // Test read if it's a SATA drive
    if (p->signature == AHCI_SIG_SATA) {
        log_write("AHCI: SATA Signature Detected", KERNEL, LOG_INFO);
        /* The DMA target needs a physical address too, so this buffer also
         * comes from the PMM rather than the heap. */
        u64 read_buf_phys = pmm_alloc_frame_below(1ull << 32);
        if (!read_buf_phys) {
            log_write("AHCI: Failed to allocate read buffer", KERNEL, LOG_ERROR);
            return -1;
        }
        memset(phys_to_virt(read_buf_phys), 0, 4096);

        if (ahci_read_sector(dev, port, 0, 1, (void *)read_buf_phys) == 0) {
            log_write("AHCI: Read sector 0 successfully!", KERNEL, LOG_INFO);
        } else {
            log_write("AHCI: Read sector 0 failed!", KERNEL, LOG_ERROR);
        }
        pmm_free_frame(read_buf_phys);
    } else if (p->signature == AHCI_SIG_ATAPI) {
        log_write("AHCI: Found ATAPI CD-ROM, skipping read test.", KERNEL, LOG_INFO);
    }

    return 0;
}

static int ahci_probe(struct device *dev) {
  struct pci_device *pci = &dev->bus_info.pci;
  log_write_string("AHCI: Found controller", "AHCI", KERNEL, LOG_INFO);

  /* Enable Memory Space (MMIO) and Bus Mastering (DMA) */
  pci_enable(pci);

  struct AHCI_DEVICE_DATA *data = kmalloc(sizeof(struct AHCI_DEVICE_DATA));
  if (!data) {
    log_write("AHCI: Failed to allocate device data", KERNEL, LOG_ERROR);
    return -1;
  }
  /* kmalloc does not zero, and ports[].is_active gates every later read. */
  memset(data, 0, sizeof(struct AHCI_DEVICE_DATA));
  dev->driver_data = data;

  struct pci_bar *abar = &pci->bar[5];
  if (!abar->valid || abar->is_io) {
    log_write("AHCI: BAR5 is invalid or not MMIO!", KERNEL, LOG_ERROR);
    kfree(data);
    return -1;
  }
  data->abar_phys = abar->base;

  /* Map ABAR into a dedicated kernel virtual address.
   * We can't use the HHDM (phys_to_virt) because the HHDM uses 1GB huge
   * pages, and we can't set VMM_PCD on a 1GB page without disabling
   * cache for all RAM in that 1GB. So we map it to a dedicated address. */

  u64 ahci_vbase = vma_alloc(abar->size);
  if (!ahci_vbase) {
      log_write("AHCI: VMA allocation failed!", KERNEL, LOG_ERROR);
      kfree(data);
      return -1;
  }

  u64 num_pages = (abar->size + 0xFFF) / 0x1000;
  if (num_pages == 0)
    num_pages = 1;

  u64 mmio_flags = VMM_PRESENT | VMM_WRITE | VMM_PCD | VMM_NX | VMM_GLOBAL;

  for (u64 i = 0; i < num_pages; i++) {
    u64 phys_page = data->abar_phys + (i * 0x1000);
    u64 virt_page = ahci_vbase + (i * 0x1000);

    if (vmm_map_in(kernel_pml4, virt_page, phys_page, mmio_flags) != 0) {
      log_write("AHCI: Failed to map ABAR!", KERNEL, LOG_ERROR);
      kfree(data);
      return -1;
    }
  }

  data->abar_virtual = (void *)ahci_vbase;

  /* AHCI Initialization Sequence (from OSDev Wiki) */
  u32 ghc = ahci_read32(data->abar_virtual, AHCI_GHC);
  if (!(ghc & AHCI_GHC_AE)) {
    ahci_write32(data->abar_virtual, AHCI_GHC, ghc | AHCI_GHC_AE);
  }

  data->ports_implemented = ahci_read32(data->abar_virtual, AHCI_PI);

  log_write_hex("AHCI: Ports Implemented =", data->ports_implemented, KERNEL,
                LOG_INFO);

  /* Port scanning loop */
  for (int i = 0; i < 32; i++) {
    if (data->ports_implemented & (1u << i)) {
      // Pass the 'data' struct, not the virtual address!
      ahci_init_port(data, i);
    }
  }
  g_ahci_dev = data;
  return 0; // Success!
}

const struct driver ahci_driver = {
    .name = "AHCI SATA Controller",
    .bus = DEVICE_BUS_PCI,
    .match = ahci_match,
    .probe = ahci_probe,
    .poll = 0,
};

void ahci_init(void) {
  log_write("AHCI: Initializing...", KERNEL, LOG_INFO);
  driver_register(&ahci_driver);
}

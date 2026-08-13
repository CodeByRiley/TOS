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

extern uint64_t *kernel_pml4;

struct AHCI_DEVICE_DATA *g_ahci_dev = NULL;

static inline void ahci_pause(void) { __asm__ volatile("pause"); }

static void ahci_write32(const volatile void *abar, uint32_t off,
                         uint32_t val) {
  *((volatile uint32_t *)((uintptr_t)abar + off)) = val;
}

static uint32_t ahci_read32(const volatile void *base, uint32_t offset) {
  return *(volatile uint32_t *)((uintptr_t)base + offset);
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
static int ahci_find_cmd_slot(volatile void *port_base, uint32_t slot_count) {
  // PxSACT (SATA Active) and PxCI (Command Issue) tell us which slots are busy
  uint32_t slots = ahci_read32(port_base, AHCI_PORT_PxCI) |
                   ahci_read32(port_base, AHCI_PORT_PxSACT);

  for (uint32_t i = 0; i < slot_count; i++) {
    if (!(slots & (1u << i))) {
      return (int)i; // Found a free slot
    }
  }
  return -1; // All slots busy
}

/* Command table for `slot`, inside the port's single-frame table array. */
static struct AHCI_CMD_TABLE *ahci_cmd_table(struct AHCI_PORT *p, int slot) {
  return (struct AHCI_CMD_TABLE *)((uintptr_t)p->cmd_table_virt +
                                   (uint32_t)slot * AHCI_CMD_TABLE_STRIDE);
}

static uint64_t ahci_cmd_table_phys(struct AHCI_PORT *p, int slot) {
  return p->cmd_table_phys + (uint64_t)(uint32_t)slot * AHCI_CMD_TABLE_STRIDE;
}

/* Clear ST and FRE, then wait for the HBA to acknowledge by dropping CR and
 * FR. Touching PxCLB/PxFB while either is still running is undefined. */
static int ahci_port_stop(volatile void *port_base) {
  uint32_t cmd = ahci_read32(port_base, AHCI_PORT_PxCMD);
  cmd &= ~(AHCI_PxCMD_ST | AHCI_PxCMD_FRE);
  ahci_write32(port_base, AHCI_PORT_PxCMD, cmd);

  for (uint32_t i = 0; i < AHCI_SPIN_LIMIT; i++) {
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
  uint32_t i;
  for (i = 0; i < AHCI_SPIN_LIMIT; i++) {
    if (!(ahci_read32(port_base, AHCI_PORT_PxCMD) & AHCI_PxCMD_CR))
      break;
    ahci_pause();
  }
  if (i == AHCI_SPIN_LIMIT)
    return -1;

  uint32_t cmd = ahci_read32(port_base, AHCI_PORT_PxCMD);
  ahci_write32(port_base, AHCI_PORT_PxCMD, cmd | AHCI_PxCMD_FRE);

  cmd = ahci_read32(port_base, AHCI_PORT_PxCMD);
  ahci_write32(port_base, AHCI_PORT_PxCMD, cmd | AHCI_PxCMD_ST);
  return 0;
}

/* The drive must have BSY and DRQ clear before a new command FIS is built. */
static int ahci_wait_ready(volatile void *port_base) {
  for (uint32_t i = 0; i < AHCI_SPIN_LIMIT; i++) {
    uint32_t tfd = ahci_read32(port_base, AHCI_PORT_PxTFD);
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

int ahci_write_sector(struct AHCI_DEVICE_DATA *dev, int port, uint64_t lba,
                      uint32_t count, void *buf_phys) {
    if (port < 0 || port >= AHCI_MAX_PORTS) return -1;
    if (count == 0 || count > 0xFFFF) return -1;

    struct AHCI_PORT *p = &dev->ports[port];
    if (!p->is_active) return -1;

    volatile void *port_base = ahci_port_base(dev->abar_virtual, port);
    ahci_clear_status(port_base);

    if (ahci_wait_ready(port_base) != 0) return -1;

    int slot = ahci_find_cmd_slot(port_base, p->slots);
    if (slot < 0) return -1;

    struct AHCI_CMD_HEADER *cmd_list = (struct AHCI_CMD_HEADER *)p->cmd_list_virt;
    struct AHCI_CMD_HEADER *cmd_hdr = &cmd_list[slot];
    memset(cmd_hdr, 0, sizeof(struct AHCI_CMD_HEADER));
    cmd_hdr->cfl = 5;
    cmd_hdr->write = 1; // 1 = WRITE to device
    cmd_hdr->prdt_length = 1;
    cmd_hdr->c = 1;

    uint64_t table_phys = ahci_cmd_table_phys(p, slot);
    cmd_hdr->command_table_base = (uint32_t)table_phys;
    cmd_hdr->command_table_base_upper = (uint32_t)(table_phys >> 32);

    struct AHCI_CMD_TABLE *cmd_table = ahci_cmd_table(p, slot);
    memset(cmd_table, 0, sizeof(struct AHCI_CMD_TABLE));

    cmd_table->prdt[0].data_base = (uint32_t)(uint64_t)buf_phys;
    cmd_table->prdt[0].data_base_upper = (uint32_t)((uint64_t)buf_phys >> 32);
    cmd_table->prdt[0].size = (count * 512) - 1;
    cmd_table->prdt[0].size |= (1u << 31);

    struct FIS_REG_H2D *fis = &cmd_table->command_fis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->command = 0x35; // 0x35 = WRITE DMA EXT

    fis->lba0 = (u8)(lba & 0xFF);
    fis->lba1 = (u8)((lba >> 8) & 0xFF);
    fis->lba2 = (u8)((lba >> 16) & 0xFF);
    fis->lba3 = (u8)((lba >> 24) & 0xFF);
    fis->lba4 = (u8)((lba >> 32) & 0xFF);
    fis->lba5 = (u8)((lba >> 40) & 0xFF);

    fis->device = 1 << 6;
    fis->countl = (u8)(count & 0xFF);
    fis->counth = (u8)((count >> 8) & 0xFF);

    ahci_write32(port_base, AHCI_PORT_PxCI, 1u << slot);

    uint32_t spins;
    for (spins = 0; spins < AHCI_SPIN_LIMIT; spins++) {
        if (!(ahci_read32(port_base, AHCI_PORT_PxCI) & (1u << slot)))
            break;
        if (ahci_read32(port_base, AHCI_PORT_PxIS) & AHCI_PxIS_TFES) {
            return -1;
        }
        ahci_pause();
    }

    if (spins == AHCI_SPIN_LIMIT) return -1;
    if (cmd_hdr->prdbc != count * 512) return -1;

    return 0;
}

/* Read `count` sectors from `lba` into physical buffer `buf_phys` */
int ahci_read_sector(struct AHCI_DEVICE_DATA *dev, int port, uint64_t lba,
                     uint32_t count, void *buf_phys) {
    if (port < 0 || port >= AHCI_MAX_PORTS) return -1;
    if (count == 0 || count > 0xFFFF) return -1;

    struct AHCI_PORT *p = &dev->ports[port];
    if (!p->is_active) return -1;

    /* One PRDT entry covers at most 4 MiB, and this path only ever programs
     * one. Anything larger needs the scatter/gather loop we do not have. */
    if ((uint64_t)count * 512 > 4u * 1024u * 1024u) return -1;

    volatile void *port_base = ahci_port_base(dev->abar_virtual, port);

    ahci_clear_status(port_base);

    if (ahci_wait_ready(port_base) != 0) {
        log_write("AHCI: drive still busy, cannot issue read", KERNEL, LOG_ERROR);
        return -1;
    }

    int slot = ahci_find_cmd_slot(port_base, p->slots);
    if (slot < 0) {
        log_write("AHCI: no free command slot", KERNEL, LOG_ERROR);
        return -1;
    }

    // Get the Command List and Command Table from our saved struct
    struct AHCI_CMD_HEADER *cmd_list = (struct AHCI_CMD_HEADER *)p->cmd_list_virt;
    struct AHCI_CMD_HEADER *cmd_hdr = &cmd_list[slot];
    memset(cmd_hdr, 0, sizeof(struct AHCI_CMD_HEADER));
    cmd_hdr->cfl = sizeof(struct FIS_REG_H2D) / sizeof(uint32_t); // 5 DWORDs
    cmd_hdr->write = 0;
    cmd_hdr->prdt_length = 1;
    cmd_hdr->c = 1;

    uint64_t table_phys = ahci_cmd_table_phys(p, slot);
    cmd_hdr->command_table_base = (uint32_t)table_phys;
    cmd_hdr->command_table_base_upper = (uint32_t)(table_phys >> 32);

    struct AHCI_CMD_TABLE *cmd_table = ahci_cmd_table(p, slot);
    memset(cmd_table, 0, sizeof(struct AHCI_CMD_TABLE));

    // Setup the PRDT
    cmd_table->prdt[0].data_base = (uint32_t)(uint64_t)buf_phys;
    cmd_table->prdt[0].data_base_upper = (uint32_t)((uint64_t)buf_phys >> 32);
    cmd_table->prdt[0].size = (count * 512) - 1;
    cmd_table->prdt[0].size |= (1u << 31); // Interrupt on completion

    // Setup the FIS
    struct FIS_REG_H2D *fis = &cmd_table->command_fis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->command = 0x25; // READ DMA EXT

    fis->lba0 = (u8)(lba & 0xFF);
    fis->lba1 = (u8)((lba >> 8) & 0xFF);
    fis->lba2 = (u8)((lba >> 16) & 0xFF);
    fis->lba3 = (u8)((lba >> 24) & 0xFF);
    fis->lba4 = (u8)((lba >> 32) & 0xFF);
    fis->lba5 = (u8)((lba >> 40) & 0xFF);

    fis->device = 1 << 6; // LBA mode
    fis->countl = (u8)(count & 0xFF);
    fis->counth = (u8)((count >> 8) & 0xFF);

    // Issue the command
    ahci_write32(port_base, AHCI_PORT_PxCI, 1u << slot);

    // Wait for completion, bailing out on a task-file error or a timeout
    // rather than spinning here forever.
    uint32_t spins;
    for (spins = 0; spins < AHCI_SPIN_LIMIT; spins++) {
        if (!(ahci_read32(port_base, AHCI_PORT_PxCI) & (1u << slot)))
            break;
        if (ahci_read32(port_base, AHCI_PORT_PxIS) & AHCI_PxIS_TFES) {
            log_write_hex("AHCI: task file error, PxTFD =",
                          ahci_read32(port_base, AHCI_PORT_PxTFD), KERNEL,
                          LOG_ERROR);
            return -1;
        }
        ahci_pause();
    }

    if (spins == AHCI_SPIN_LIMIT) {
        log_write_hex("AHCI: read timed out, PxTFD =",
                      ahci_read32(port_base, AHCI_PORT_PxTFD), KERNEL, LOG_ERROR);
        return -1;
    }

    if (ahci_read32(port_base, AHCI_PORT_PxTFD) & AHCI_PxTFD_STS_ERR) {
        log_write("AHCI: ATA error reported after read", KERNEL, LOG_ERROR);
        return -1;
    }

    if (cmd_hdr->prdbc != count * 512) {
        log_write_hex("AHCI: short read, bytes transferred =", cmd_hdr->prdbc,
                      KERNEL, LOG_ERROR);
        return -1;
    }

    return 0;
}

static int ahci_init_port(struct AHCI_DEVICE_DATA *dev, int port) {
    void *abar_virtual = dev->abar_virtual;
    volatile void *port_base = ahci_port_base(abar_virtual, port);
    struct AHCI_PORT *p = &dev->ports[port];

    // Check if a device is actually present (PxSSTS DET == 3)
    uint32_t ssts = ahci_read32(port_base, AHCI_PORT_PxSSTS);
    if ((ssts & AHCI_PxSSTS_DET_MASK) != AHCI_PxSSTS_DET_PRESENT) {
        return -1; // No drive plugged in
    }

    // Check Signature
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
     * in the HHDM, so virt_to_phys() is meaningless on a kmalloc pointer —
     * allocate raw frames and address them through the HHDM instead.
     *
     * A frame base is 4 KiB aligned, which satisfies the 1 KiB command-list
     * and 256-byte received-FIS alignment rules at offsets 0 and 1024. */
    uint64_t clb_frame = pmm_alloc_frame();
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
    uint64_t ctba_frame = pmm_alloc_frame();
    if (!ctba_frame) {
        log_write("AHCI: no frame for command tables", KERNEL, LOG_ERROR);
        pmm_free_frame(clb_frame);
        return -1;
    }
    p->cmd_table_phys = ctba_frame;
    p->cmd_table_virt = phys_to_virt(ctba_frame);
    memset(p->cmd_table_virt, 0, 4096);
    /* CAP.NCS holds the slot count minus one. */
    uint32_t ncs = ((ahci_read32(abar_virtual, AHCI_CAP) >> AHCI_CAP_NCS_SHIFT) &
                    AHCI_CAP_NCS_MASK) + 1;
    p->slots = ncs < AHCI_SLOTS_PER_PORT ? ncs : AHCI_SLOTS_PER_PORT;

    ahci_write32(port_base, AHCI_PORT_PxCLB, (uint32_t)p->cmd_list_phys);
    ahci_write32(port_base, AHCI_PORT_PxCLBU, (uint32_t)(p->cmd_list_phys >> 32));
    ahci_write32(port_base, AHCI_PORT_PxFB, (uint32_t)p->fis_phys);
    ahci_write32(port_base, AHCI_PORT_PxFBU, (uint32_t)(p->fis_phys >> 32));
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
        uint64_t read_buf_phys = pmm_alloc_frame();
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

  /* Allocate private driver data */
  struct AHCI_DEVICE_DATA *data = kmalloc(sizeof(struct AHCI_DEVICE_DATA));
  if (!data) {
    log_write("AHCI: Failed to allocate device data", KERNEL, LOG_ERROR);
    return -1;
  }
  /* kmalloc does not zero, and ports[].is_active gates every later read. */
  memset(data, 0, sizeof(struct AHCI_DEVICE_DATA));
  dev->driver_data = data;

  /* Get BAR5 (AHCI Base Address Register) */
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

  uint64_t ahci_vbase = vma_alloc(abar->size);
  if (!ahci_vbase) {
      log_write("AHCI: VMA allocation failed!", KERNEL, LOG_ERROR);
      kfree(data);
      return -1;
  }

  uint64_t num_pages = (abar->size + 0xFFF) / 0x1000;
  if (num_pages == 0)
    num_pages = 1;

  uint64_t mmio_flags = VMM_PRESENT | VMM_WRITE | VMM_PCD | VMM_NX | VMM_GLOBAL;

  for (uint64_t i = 0; i < num_pages; i++) {
    uint64_t phys_page = data->abar_phys + (i * 0x1000);
    uint64_t virt_page = ahci_vbase + (i * 0x1000);

    if (vmm_map_in(kernel_pml4, virt_page, phys_page, mmio_flags) != 0) {
      log_write("AHCI: Failed to map ABAR!", KERNEL, LOG_ERROR);
      kfree(data);
      return -1;
    }
  }

  data->abar_virtual = (void *)ahci_vbase;

  /* AHCI Initialization Sequence (from OSDev Wiki) */
  uint32_t ghc = ahci_read32(data->abar_virtual, AHCI_GHC);
  if (!(ghc & AHCI_GHC_AE)) {
    ahci_write32(data->abar_virtual, AHCI_GHC, ghc | AHCI_GHC_AE);
  }

  // Read Ports Implemented (PI) to know which ports to scan
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

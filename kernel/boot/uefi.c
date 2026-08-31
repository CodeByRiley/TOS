/* kernel/boot/uefi.c -- decode EFI-related Multiboot2 information tags. */
#include <boot/multiboot2.h>
#include <boot/uefi.h>
#include <utilities/log.h>

static struct uefi_boot_info boot_info;

void uefi_init(u64 mb2_addr) {
  struct MB2_TAG_EFI64_PTR *system64 =
      (struct MB2_TAG_EFI64_PTR *)mb2_find_tag(mb2_addr,
                                               MULTIBOOT_TAG_EFI64);
  struct MB2_TAG_EFI32_PTR *system32 =
      (struct MB2_TAG_EFI32_PTR *)mb2_find_tag(mb2_addr,
                                               MULTIBOOT_TAG_EFI32);

  if (system64) {
    boot_info.present = true;
    boot_info.is_64_bit = true;
    boot_info.system_table = system64->pointer;
  } else if (system32) {
    boot_info.present = true;
    boot_info.system_table = system32->pointer;
  } else {
    log_write("BOOT: legacy BIOS Multiboot2 handoff", KERNEL, LOG_INFO);
    return;
  }

  boot_info.boot_services_active =
      mb2_find_tag(mb2_addr, MULTIBOOT_TAG_EFI_BS) != 0;

  if (boot_info.is_64_bit) {
    struct MB2_TAG_EFI64_PTR *image =
        (struct MB2_TAG_EFI64_PTR *)mb2_find_tag(mb2_addr,
                                                 MULTIBOOT_TAG_EFI64_IH);
    if (image)
      boot_info.image_handle = image->pointer;
  } else {
    struct MB2_TAG_EFI32_PTR *image =
        (struct MB2_TAG_EFI32_PTR *)mb2_find_tag(mb2_addr,
                                                 MULTIBOOT_TAG_EFI32_IH);
    if (image)
      boot_info.image_handle = image->pointer;
  }

  struct MB2_TAG_EFI_MMAP *map =
      (struct MB2_TAG_EFI_MMAP *)mb2_find_tag(mb2_addr,
                                              MULTIBOOT_TAG_EFI_MMAP);
  if (map && map->size >= sizeof(*map)) {
    boot_info.memory_map = map->descriptors;
    boot_info.memory_map_size = map->size - sizeof(*map);
    boot_info.descriptor_size = map->descriptor_size;
    boot_info.descriptor_version = map->descriptor_version;
  }

  log_write(boot_info.is_64_bit ? "BOOT: UEFI64 Multiboot2 handoff"
                                : "BOOT: UEFI32 Multiboot2 handoff",
            KERNEL, LOG_INFO);
  log_write_hex("UEFI: system table =", boot_info.system_table, KERNEL,
                LOG_INFO);
  if (boot_info.image_handle)
    log_write_hex("UEFI: image handle =", boot_info.image_handle, KERNEL,
                  LOG_INFO);
  log_write(boot_info.boot_services_active
                ? "UEFI: boot services are still active"
                : "UEFI: boot services exited by GRUB",
            KERNEL, LOG_INFO);
  if (boot_info.memory_map) {
    log_write_hex("UEFI: memory map bytes =", boot_info.memory_map_size,
                  KERNEL, LOG_INFO);
    log_write_hex("UEFI: descriptor size =", boot_info.descriptor_size,
                  KERNEL, LOG_INFO);
    log_write_hex("UEFI: descriptor version =", boot_info.descriptor_version,
                  KERNEL, LOG_INFO);
  }
}

bool uefi_booted(void) { return boot_info.present; }

const struct uefi_boot_info *uefi_get_boot_info(void) { return &boot_info; }

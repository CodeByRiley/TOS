/* kernel/boot/uefi.h -- UEFI metadata carried by the Multiboot2 handoff.
 *
 * GRUB owns the UEFI boot-services phase. By the time the kernel runs those
 * services are normally gone, but the system table, image handle and EFI
 * memory-map metadata are still valuable for diagnostics and future runtime
 * service support.
 */
#ifndef BOOT_UEFI_H
#define BOOT_UEFI_H

#include <stdint.h>
#include <utilities/types.h>

struct uefi_boot_info {
  bool present;
  bool is_64_bit;
  bool boot_services_active;
  u64 system_table;
  u64 image_handle;
  const void *memory_map;
  u32 memory_map_size;
  u32 descriptor_size;
  u32 descriptor_version;
};

void uefi_init(u64 mb2_addr);
bool uefi_booted(void);
const struct uefi_boot_info *uefi_get_boot_info(void);

#endif

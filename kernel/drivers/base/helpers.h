#ifndef DRIVER_HELPERS_H
#define DRIVER_HELPERS_H

#include <utilities/types.h>

SINLINE u8 read8(uintptr_t addr) { return *(volatile u8 *)addr; }

SINLINE u16 read16(uintptr_t addr) { return *(volatile u16 *)addr; }

SINLINE u32 read32(uintptr_t addr) { return *(volatile u32 *)addr; }

SINLINE void write8(uintptr_t addr, u8 value) { *(volatile u8 *)addr = value; }

SINLINE void write16(uintptr_t addr, u16 value) {
  *(volatile u16 *)addr = value;
}

SINLINE void write32(uintptr_t addr, u32 value) {
  *(volatile u32 *)addr = value;
}

SINLINE u64 mmio_read64_split(uintptr_t addr) {
  u32 low = read32(addr);
  u32 high = read32(addr + sizeof(u32));

  return ((u64)high << 32) | (u64)low;
}

SINLINE void mmio_write64_split(uintptr_t addr, u64 value) {
  write32(addr, (u32)value);
  write32(addr + 4, (u32)(value >> 32));
}

#endif

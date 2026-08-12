# TOS

A hobby x86_64 operating system: long-mode kernel, SMP bring-up, FAT32 root filesystem with support FAT16.
preemptive scheduler, syscall ABI, framebuffer window manager, and a small prebuilt userspace including a ported version of DOOM

## Layout

```
kernel/              Kernel. Headers live next to the sources that implement them.
  main.c             Boot entry after the asm stubs hand off.
  acpi/              ACPI table + MCFG parsing
  arch/              CPU state: GDT, TSS, per-CPU, syscall dispatch
  arch/x86_64/       Assembly: boot stubs, ISR stubs, context switch, SYSCALL entry
  boot/              Multiboot2 tag walk
  devices/           LAPIC, PIT, serial, port I/O
  display/           Framebuffer, graphics, TTY, text print
  drivers/           Driver registry; drivers/video/ holds the NVIDIA GSP driver
  firmware/          Firmware blob loading
  fs/                FAT16 driver + kernel stdio
  input/             Keyboard, mouse
  interrupts/        IDT, PIC
  loader/            ELF loader, process creation
  memory/            PMM, VMM, heap, HHDM
  msg/               Per-task message rings
  pci/               PCI enumeration
  sched/             Scheduler, SMP bring-up
  sync/              Spinlocks
  utilities/         string, printf, stdlib, logging
  virtio/            virtio-pci transport + virtio-gpu

userspace/
  bin/<name>/        One directory per binary, built to <name>.elf
  lib/               libc subset, crt0, syscall stubs, window-manager client
  include/           Userspace headers
  games/doom/        doomgeneric port + platform glue

boot/x86_64/         Linker script and GRUB ISO staging tree
rootfs/              Files copied into the FAT16 disk image
tools/               Build and run scripts
tests/               Host-side unit tests
build/  dist/        Generated. Both are gitignored; `make clean` removes them.
```

Kernel includes are written relative to `kernel/`, e.g. `#include "fs/fat.h"`.
Userspace gets the same root via `-I ../kernel` so it can share the syscall ABI
header.

## Build

Requires an `x86_64-elf` cross toolchain, `nasm`, and WSL with `grub-mkimage`,
`xorriso`, `mtools`, and `dosfstools` for the disk image and ISO.

```bash
mingw32-make -j12 build-x86_64
```

Faster inner loop — compile and link the kernel only, no disk image or ISO:

```bash
mingw32-make -j12 kernel
```

Then run it:

```bash
tools/run.bat
```

`tools/build.bat` and `tools/run.sh` are the same steps for cmd and bash. All
scripts cd to the repository root themselves, so they work from any directory.

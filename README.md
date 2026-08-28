# TOS

TOS is an experimental x86_64 operating system built from scratch. It boots with GRUB/Multiboot2, runs user programs in ring 3, and includes a graphical desktop alongside a growing set of Linux-compatible system calls.

Most user programs are statically linked with musl. TOS-specific features—windows, graphics, audio, IPC, and process inspection—are provided by `libtos`.

TOS is mainly intended for QEMU and OS-development experiments. It is not a general-purpose or security-hardened operating system.

## What works

- **Kernel and processes:** x86_64 long mode, ACPI discovery, LAPIC-based SMP startup, an AP work queue, priority-aware preemptive scheduling, ring-3 processes, asynchronous process creation, native user threads, futexes, IPC, and shared memory.
- **Memory:** physical and virtual memory managers, a kernel heap, per-process page tables, demand-paged virtual memory regions, and the `mmap`, `mprotect`, and `munmap` paths required by static musl programs. `brk` is intentionally rejected so musl uses `mmap` instead.
- **Executables:** static ELF64 and PE32+ loaders. Builds produce ELF programs by default, along with PE versions of `hello` and `ls` to test both loaders.
- **Storage:** a writable, case-insensitive FAT16/FAT32 filesystem with VFAT long filenames. The kernel can mount an AHCI volume or use the FAT32 ramdisk loaded by GRUB.
- **Graphics and input:** Multiboot framebuffer fallback, virtio-gpu scanout and resize support, TTF text rendering, keyboard and mouse input, and the Winman desktop. Winman supports movable and resizable windows, launchers, and a taskbar clock. NVIDIA GSP display support is also included but still under development and has not yet been tested on physical hardware.
- **Drivers:** PCI discovery and driver registration, AHCI storage, Intel e1000 networking, Sound Blaster 16 audio, UHCI USB, virtio-gpu, and the developing NVIDIA driver.
- **Networking:** Ethernet, ARP, IPv4, ICMP echo, UDP, packet capture and statistics, plus the Linux x86_64 syscall numbers for `socket`, `bind`, `sendto`, and `recvfrom`.
- **Userspace:** a shell with `PATH` lookup, tab completion, working directories, built-ins, and background jobs; command-line utilities; graphical demos and tools; Netmon; ping; a UDP echo program; a DOOM port; and the HolyD bytecode language and runtime.
- **Tests:** fast host-side tests and QEMU tests covering SMP, virtual memory, process lifetime, FAT, ELF/PE loading, musl, Winman, framebuffer ownership, networking, `PATH` lookup, and kernel panics.

## Quick start

The supported build environment is Windows with MSYS2/MinGW tools and WSL.

You will need:

- `mingw32-make` and MSYS2 `bash` on `PATH`. The build recipes use commands such as `mkdir -p`, `&&`, and shell-style quoting, so `make` must be able to find a POSIX shell.
- Host `gcc` for the host test suite.
- `x86_64-w64-mingw32-gcc` for PE user programs.
- An `x86_64-elf` GCC/binutils cross-toolchain containing `gcc`, `ld`, `ar`, and `objcopy`. The kernel uses `-std=gnu23`, so the cross-compiler must be GCC 14 or newer.
- NASM.
- Python on Windows. The kernel build always runs `tools/gen_asm_offsets.py` and `tools/gen_symtab.py`; the `clangd` target also needs Python.
- WSL with `grub-mkimage` (BIOS/i386-pc modules, not EFI), `xorriso`, `mcopy`/`mmd`, and `mkfs.fat`. On Ubuntu that is `sudo apt install grub-pc-bin grub-common xorriso mtools dosfstools`. Disk-image creation, ISO creation, and cleaning run through `wsl bash`.
- QEMU (`qemu-system-x86_64`) to run TOS.
- `python3` inside WSL for the QEMU test suite.

Two things trip people up on Windows:

- **Use `mingw32-make`, not MSYS2's own `make`.** MSYS2's `make` reports a `/c/...` working directory that `wslpath` converts to the wrong place; the build stops with an error saying so.
- **The disk-image, ISO, and clean steps always run in WSL, even when you start the build from an MSYS2 or Git Bash shell.** MSYS2 packages neither `xorriso` nor a usable i386-pc GRUB, so running them "natively" just picks up whatever unrelated `grub-mkimage` is on `PATH` and fails later with errors about `moddep.lst` or `kernel.img is miscompiled`. Set `TOS_NATIVE_TOOLS=1` only if your shell genuinely has the whole set.

On Linux everything runs natively and no WSL is involved.

Clone TOS and its HolyD submodule:

```powershell
git clone --recursive https://github.com/CodeByRiley/TOS.git
cd TOS
```

If you already cloned the repository without its submodules:

```powershell
git submodule update --init --recursive
```

Build everything with:

```powershell
mingw32-make -j12 build-x86_64
```

The build has four main parts:

### Kernel

`kernel/arch/offsets.c` is compiled with `-S`. The generated assembly is processed by `tools/gen_asm_offsets.py`, which creates:

```text
build/generated/asm_offsets.inc
```

This file contains constants shared by C structs and hand-written assembly.

Kernel `.c` and `.asm` files are compiled into `build/kernel/`. The AP trampoline is assembled as a flat binary and wrapped in an ELF object. The kernel copies it into low memory before sending the INIT-SIPI-SIPI sequence to additional CPUs.

### Linking

The kernel is linked twice:

1. The first link creates `build/kernel_nosyms.elf`.
2. `tools/gen_symtab.py` generates the kernel symbol table as C source.
3. That source is compiled and linked into the final kernel:

```text
dist/x86_64/kernel.bin
```

### Userspace

The `userspace/` sub-build bootstraps musl when needed and builds the programs and `libtos`.

It supports these options:

- `BUILD_DOOM`
- `BUILD_NETSURF`
- `USERSPACE_CLEAN`

### Disk image and ISO

`tools/create_disk.sh` runs inside WSL and creates the minimum 64 MiB FAT32 disk image:

```text
build/disk.img
```

`tools/build_iso.sh` then populates the ISO staging directory under `boot/x86_64/iso/`, uses `grub-mkimage` to create the El Torito boot image, and uses `xorriso` to package:

```text
dist/x86_64/kernel.iso
```

The script looks for the i386-pc GRUB modules in the usual places (`/usr/lib/grub/i386-pc` and friends, plus the directory the `grub-mkimage` on `PATH` lives in), accepting only a directory that holds both `moddep.lst` and `cdboot.img`. If your GRUB is somewhere else, point it there:

```bash
GRUB_DIR=/path/to/grub/i386-pc bash tools/build_iso.sh
```

`grub-mkimage` and that directory must come from the same GRUB install. A mismatch surfaces as a missing `moddep.lst` or as `kernel.img is miscompiled: its start address is 0x0`.

Run TOS with the full QEMU setup:

```powershell
.\tools\run.bat
```

This enables virtio graphics, e1000 networking, SB16 audio, and USB tablet input.

The main generated files are:

```text
dist/x86_64/kernel.bin   linked kernel
dist/x86_64/kernel.iso   bootable GRUB ISO
build/disk.img           FAT32 root filesystem
```

`build-x86_64` also copies the kernel, disk image, and El Torito image into `boot/x86_64/iso/`. The `clean` target removes those copies as well.

`tools/build.bat` wraps the normal build. `tools/run.sh` provides a smaller Bash/QEMU launcher for environments where the Windows launcher is not suitable.

## Build targets

```powershell
# Build the kernel and perform the two-pass link.
# Does not build userspace, the disk image, or the ISO.
mingw32-make -j12 kernel

# Build userspace, including musl, programs, and libtos.
# Does not build the kernel, disk image, or ISO.
mingw32-make userspace

# Force a clean userspace rebuild on the next full build.
mingw32-make -j12 build-x86_64 USERSPACE_CLEAN=1

# Build everything: kernel, userspace, disk image, and bootable ISO.
mingw32-make -j12 build-x86_64

# Include the optional DOOM payload.
mingw32-make -j12 build-x86_64 BUILD_DOOM=1

# Include the optional NetSurf payload.
mingw32-make -j12 build-x86_64 BUILD_NETSURF=1

# Regenerate compile_commands.json for clangd.
mingw32-make clangd

# Build HolyD as a native Windows program.
mingw32-make holyd-win

# Remove build/, dist/, ISO staging files, and userspace output.
mingw32-make clean
```

`holyd-win` produces:

```text
userspace/bin/holyd/holyd.exe
```

Run it from that directory, for example:

```powershell
./holyd.exe samples/gui.hd
```

DOOM and NetSurf are optional because their source or resource files are not included in a normal clean checkout. If their binaries and assets are available, the disk-image builder packages them automatically.

A normal build does not require DOOM WADs, music, NetSurf resources, or NVIDIA firmware.

## Using TOS

When it starts, TOS launches Winman and the Shelf shell.

Type `help` to see the shell's built-in commands. External programs can be run without their `.elf` suffix. Add `&` to the end of a command to run it in the background and keep using the shell.

Examples:

```text
help
ls /
tree /
hello
btop
notepad &
uidemo &
netmon &
ping 10.0.2.2
holyd holyd/samples/gui.hd
```

The root filesystem uses these executable directories:

- `/usr/bin` — normal applications and development tools
- `/system/bin` — the shell, Winman, core utilities, and HolyD
- `/bin` and `/usr/local/bin` — standard locations reserved for future or locally installed programs

The default `PATH` is:

```text
/bin:/usr/bin:/usr/local/bin:/system/bin
```

A command containing `/` is run directly instead of being searched for in `PATH`. You can change the current shell's search path with:

```text
PATH=...
export PATH=...
```

## Userspace and compatibility

Regular ELF programs use musl 1.2.6 as their C library.

The kernel currently implements the subset of the Linux x86_64 syscall ABI needed by static programs. This includes core file operations and metadata, directory iteration, virtual memory, time, polling, TLS setup, process exit, and UDP sockets.

Where possible, programs use normal POSIX headers and APIs.

TOS-specific features are provided by:

```text
userspace/lib/libtos.a
```

These programs are not included by default

- [DoomGeneric](https://github.com/ozkl/doomgeneric)

	Note: Doom Generic does not provide any DOOM assets, you will need to provide DOOM1.wad
	
- [NetSurf](https://www.netsurf-browser.org/)

`libtos` handles the framebuffer, Winman IPC, console I/O, drawing, fonts, audio, input, and system inspection.

An older hand-written libc is still used by programs that cannot yet use musl, including the native TOS thread test and the DOOM port. PE builds use a separate MinGW-compatible startup and syscall layer.

HolyD is maintained in the [HolyD repository](https://github.com/CodeByRiley/HolyD) and included here as `userspace/bin/holyd`. Its lexer, parser, bytecode compiler, VM, windowing FFI, and UDP FFI are built into the TOS image. The same scripts can also run in the standalone Windows build.

More information is available in:

- [`userspace/libc/README.md`](userspace/libc/README.md) — the musl port
- [`userspace/bin/holyd/README.md`](userspace/bin/holyd/README.md) — the HolyD language and standalone build

## Networking

The QEMU launcher configures one e1000 interface with a static guest address:

```text
Guest:            10.0.2.30/24
Gateway:          10.0.2.2
UDP forwarding:   host port 5000 -> guest port 5000
```

This command exercises outbound ARP, IPv4, ICMP, and the e1000 transmit/receive path:

```text
ping 10.0.2.2
```

`netmon` displays interface counters and captured Ethernet traffic. `udpecho` uses musl's normal socket API. The kernel also starts a UDP echo service on port 5000 for host-to-guest testing.

Networking is intentionally limited for now:

- one network interface
- static addressing
- polled `recvfrom`
- no DHCP
- no DNS
- no TCP
- no IPv6
- several socket operations still missing

## Tests

The fast test suite builds and runs on the Windows host.

```powershell
mingw32-make test-host
```

`mingw32-make test` is an alias for the same target.

The suite builds nine binaries in `build/tests/` and runs them with `set -e`, stopping at the first failure. It covers:

- physical and virtual memory managers
- per-process page tables
- FAT directories
- stdio modes
- BMP decoding
- graphics and UI helpers
- framebuffer damage tracking
- the HolyD compiler

You can override the host compiler with `HOST_CC`:

```powershell
mingw32-make test-host HOST_CC=gcc
```

The QEMU suite first rebuilds the full image and then runs TOS under `qemu-system-x86_64` inside WSL:

```powershell
mingw32-make test-qemu-heavy
```

It runs fifteen scripts covering:

- SMP asynchronous process creation
- a four-CPU system stress test
- window lifetime
- Muse liveness
- framebuffer mapping lifetime
- virtio-gpu resizing
- Deskelf
- Netmon
- ARP
- ping
- UDP
- Winman partial repaint
- Winman title-bar double-click handling
- `PATH` lookup
- the kernel panic screen

Run both the host and QEMU suites with:

```powershell
mingw32-make test-heavy
```

The QEMU suite requires `python3` and `qemu-system-x86_64` inside WSL.

Individual scripts in `tests/` accept their own options, including:

```text
--timeout
--cpus
--boot-timeout
```

These are useful when working on a single subsystem or testing configurations with slower boot times.

## Repository layout

```text
kernel/
  arch/             GDT/TSS, per-CPU state, syscalls, and x86_64 assembly
  boot/             Multiboot2 parsing
  devices/          LAPIC, PIT, serial, and USB orchestration
  display/          framebuffer, graphics, TTY, and fonts
  drivers/          driver core, storage, networking, sound, USB, and video
  fs/               FAT16/FAT32 and kernel stdio
  input/            keyboard and mouse handling
  interrupts/       IDT and PIC
  loader/           ELF, PE, and process creation
  memory/           PMM, VMM, demand paging, VMAs, heap, and HHDM
  msg/              input messages, IPC, and shared memory
  net/              Ethernet, ARP, IPv4, ICMP, UDP, sockets, and Netmon
  sched/            tasks, preemption, SMP startup, and the AP work queue

userspace/
  bin/              application directories; HolyD is a submodule
  games/doom/       doomgeneric port and TOS platform code
  include/          legacy userspace headers
  lib/              libtos, legacy libc code, startup, and syscall stubs
  libc/             musl 1.2.6 source, build scripts, and compatibility notes
  netsurf_compat/   compatibility layer for the experimental NetSurf port

boot/x86_64/        linker script and GRUB ISO staging tree
rootfs/             fonts, icons, wallpaper, firmware, and optional payloads
tools/              build, disk-image, run, and symbol-generation tools
tests/              host unit tests and QEMU integration tests
build/ dist/        generated output; removed by mingw32-make clean
```

Kernel headers are included relative to `kernel/`, for example:

```c
#include <fs/fat.h>
```

Userspace also includes the repository root when it needs to share an ABI definition. New musl programs should use standard headers whenever possible and include TOS syscall headers only for services without a POSIX equivalent.

## Current limits

The musl integration is a compatibility layer, not a complete Linux environment.

The following areas are still unfinished or incomplete:

- signals
- `clone`-based pthreads
- pipes
- several filesystem calls
- full terminal behavior
- many socket operations
- USB host-controller support
- hardware graphics support

See [`TODO.txt`](TODO.txt) for the current roadmap and the reasoning behind the next milestones.

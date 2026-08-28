# TOS

TOS is an experimental x86_64 operating system built from scratch. It boots
through GRUB/Multiboot2, runs a preemptive ring-3 userspace, and combines a
graphical desktop with a small but growing Linux-compatible syscall surface.
Most userspace programs are statically linked against musl; TOS-specific
services such as windows, graphics, audio, IPC, and process inspection live in
`libtos`.

The project is designed primarily for QEMU and OS-development experiments. It
is not a general-purpose or security-hardened operating system.

## What works

- **Kernel and processes:** x86_64 long mode, ACPI discovery, LAPIC-based SMP
  bring-up, an AP work queue, a priority-aware preemptive scheduler, ring-3
  processes, asynchronous spawning, native user threads, futexes, IPC, and
  shared memory.
- **Memory:** physical and virtual memory managers, a kernel heap, per-process
  page tables, demand-paged virtual regions, and the `mmap`, `mprotect`,
  and `munmap` paths needed by static musl programs. `brk` is deliberately
  rejected so musl falls back to `mmap`.
- **Executables:** static ELF64 and PE32+ loaders. The build produces ELF
  programs by default and PE variants of `hello` and `ls` to exercise both
  loaders.
- **Storage:** a writable, case-insensitive FAT16/FAT32 filesystem with VFAT
  long filenames. The kernel can mount an AHCI-backed volume or fall back to
  the FAT32 ramdisk loaded by GRUB.
- **Graphics and input:** Multiboot framebuffer fallback, virtio-gpu scanout
  and resize support, TTF text rendering, keyboard and mouse input, and the
  Winman compositing desktop with movable/resizable windows, launchers, and a
  taskbar clock. NVIDIA GSP-backed display work also lives in the tree and is
  being developed, do not currently have a spare computer to test it on.
- **Drivers:** PCI discovery and a driver registry, AHCI storage, Intel e1000
  networking, Sound Blaster 16 audio, UHCI USB, virtio-gpu, and the developing
  NVIDIA path.
- **Networking:** Ethernet, ARP, IPv4, ICMP echo, UDP, packet capture/stats,
  and the Linux x86_64 `socket`, `bind`, `sendto`, and `recvfrom` syscall
  numbers used directly by musl.
- **Userspace:** a shell with `PATH` lookup, tab completion, working
  directories, built-ins, and background jobs; command-line utilities;
  graphical demos and tools; Netmon; ping; a UDP echo program; a DOOM port;
  and the HolyD bytecode language and runtime.
- **Regression coverage:** fast host tests plus QEMU tests for SMP, VM and
  process lifetime, FAT, ELF/PE loading, musl, Winman, framebuffer ownership,
  networking, PATH lookup, and kernel panics.

## Quick start

The supported build path is Windows with MSYS2/MinGW tools and WSL. You need:

- `mingw32-make`, MSYS2 `bash`, host `gcc`, and
  `x86_64-w64-mingw32-gcc`;
- an `x86_64-elf` GCC/binutils cross-toolchain, including `gcc`, `ld`, `ar`,
  and `objcopy`;
- NASM;
- WSL with `grub-mkimage`, `xorriso`, `mcopy`/`mmd`, and `mkfs.fat`;
- QEMU (`qemu-system-x86_64`) to run the result.

Clone the HolyD submodule along with TOS:

```powershell
git clone --recursive https://github.com/CodeByRiley/TOS.git
cd TOS
```

For an existing clone made without `--recursive`:

```powershell
git submodule update --init --recursive
```

The normal build compiles the kernel, bootstraps musl when necessary, builds
userspace, creates a minimum 64 MiB FAT32 image, and packages a bootable ISO:

```powershell
mingw32-make -j12 build-x86_64
```

Run it with the complete QEMU configuration, including virtio graphics, e1000
networking, SB16 audio, and USB tablet input:

```powershell
.\tools\run.bat
```

Generated artifacts are written to:

```text
dist/x86_64/kernel.bin   linked kernel
dist/x86_64/kernel.iso   bootable GRUB ISO
build/disk.img           FAT32 root filesystem
```

`tools/build.bat` wraps the normal build, while `tools/run.sh` provides a
smaller Bash/QEMU launch for environments where the Windows launcher is not
appropriate.

## Build targets

```powershell
# Kernel compile/link only: fastest kernel development loop
mingw32-make -j12 kernel

# Rebuild the bootable image
mingw32-make -j12 build-x86_64

# Include the optional DOOM port
mingw32-make -j12 build-x86_64 BUILD_DOOM=1

# Include the experimental NetSurf port
mingw32-make -j12 build-x86_64 BUILD_NETSURF=1

# Build the HolyD submodule as a native Windows program
mingw32-make holyd-win

# Regenerate compile_commands.json for clangd
mingw32-make clangd

# Remove generated kernel, ISO, disk, and userspace build output
mingw32-make clean
```

DOOM and NetSurf are opt-in because their source/resource payloads are not
part of a normal clean checkout. The disk builder packages them automatically
when their binaries and assets exist. The standard build remains bootable
without optional WADs, music, NetSurf resources, or NVIDIA firmware.

## Inside TOS

The kernel starts Winman and the Shelf shell. Type `help` for shell built-ins;
external commands may be entered without their `.elf` suffix. A trailing `&`
runs a program asynchronously so the prompt remains available.

Some useful commands are:

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

The root image uses these executable locations:

- `/usr/bin` for normal applications and development/test tools;
- `/system/bin` for the shell, Winman, core utilities, and HolyD;
- `/bin` and `/usr/local/bin` as standard search locations for future or
  locally installed programs.

The default shell `PATH` is
`/bin:/usr/bin:/usr/local/bin:/system/bin`. Paths containing `/` bypass the
search, and `PATH=...` or `export PATH=...` changes the shell's current search
list.

## Userspace and compatibility

musl 1.2.6 is the default C library for regular ELF programs. The kernel
implements the subset of the Linux x86_64 syscall ABI currently needed for
static programs: core file I/O and metadata, directory iteration, virtual
memory, time, polling, TLS setup, process exit, and UDP sockets. Programs use
ordinary POSIX headers and calls where that ABI exists.

`userspace/lib/libtos.a` supplies services that are intentionally TOS-specific:
the framebuffer, Winman IPC, console I/O, drawing, fonts, audio, input, and
system inspection. The older hand-written libc remains only for programs that
cannot yet use musl, notably the native TOS thread test and the DOOM port. The
PE builds use a separate MinGW-compatible startup and syscall layer.

HolyD is maintained in
[`CodeByRiley/HolyD`](https://github.com/CodeByRiley/HolyD) and consumed here
as `userspace/bin/holyd`. Its lexer, parser, bytecode compiler, VM, windowing
FFI, and UDP FFI build into the TOS image; the same scripts can run in the
standalone Windows host.

For details on the musl port, see
[`userspace/libc/README.md`](userspace/libc/README.md). For HolyD's language
and standalone build, see
[`userspace/bin/holyd/README.md`](userspace/bin/holyd/README.md).

## Networking status

The QEMU launcher configures one e1000 interface with a static guest address:

```text
Guest       10.0.2.30/24
Gateway     10.0.2.2
UDP forward host:5000 -> guest:5000
```

`ping 10.0.2.2` exercises outbound ARP, IPv4, ICMP, and the e1000 TX/RX path.
`netmon` displays interface counters and captured Ethernet traffic. `udpecho`
uses musl's standard socket API, and the kernel also starts a UDP echo service
on port 5000 for host-to-guest testing.

Networking is still intentionally small: there is one interface, the address
is static, `recvfrom` is polled, and DHCP, DNS, TCP, IPv6, and the remaining
socket operations are not implemented yet.

## Tests

The fast suite builds and runs on the host:

```powershell
mingw32-make test-host
```

It covers the physical and virtual memory managers, per-process page tables,
FAT directories, stdio modes, BMP decoding, graphics/UI helpers, framebuffer
damage tracking, and the HolyD compiler.

The full suite runs the host tests, rebuilds the ISO, and drives TOS under
QEMU:

```powershell
mingw32-make test-heavy
```

The heavy target additionally requires `python3` and `qemu-system-x86_64`
inside WSL. It exercises SMP work, asynchronous process loading, resource
cleanup under stress, Winman/window lifetimes, framebuffer mappings and
resize, Deskelf, Netmon, ARP, ICMP, UDP, repaint behavior, PATH lookup, and the
panic screen.

Individual QEMU tests under `tests/` accept their own timeout and CPU-count
options and are useful while developing one subsystem. `mingw32-make test` is
an alias for the fast host suite.

## Repository layout

```text
kernel/
  arch/             GDT/TSS, per-CPU state, syscalls, x86_64 assembly
  boot/             Multiboot2 parsing
  devices/          LAPIC, PIT, serial, USB orchestration
  display/          framebuffer, graphics, TTY, and fonts
  drivers/          driver core; storage, network, sound, USB, and video
  fs/               FAT16/FAT32 and kernel stdio
  input/            keyboard and mouse
  interrupts/       IDT and PIC
  loader/           ELF, PE, and process creation
  memory/           PMM, VMM, VMA/demand paging, heap, and HHDM
  msg/              input messages, IPC, and shared-memory plumbing
  net/              Ethernet, ARP, IPv4, ICMP, UDP, sockets, and Netmon
  sched/             tasks, preemption, SMP bring-up, and the AP work queue

userspace/
  bin/               one directory per application; HolyD is a submodule
  games/doom/        doomgeneric port and TOS platform glue
  include/           legacy userspace headers
  lib/               libtos, legacy libc pieces, startup, and syscall stubs
  libc/              musl 1.2.6 source, build scripts, and compatibility notes
  netsurf_compat/    compatibility layer for the experimental NetSurf port

boot/x86_64/         linker script and GRUB ISO staging tree
rootfs/              fonts, icons, wallpaper, firmware, and optional payloads
tools/               build, disk-image, run, and symbol-generation helpers
tests/               host unit tests and QEMU integration/regression tests
build/  dist/        generated output; removed by `mingw32-make clean`
```

Kernel headers are included relative to `kernel/`, for example
`#include <fs/fat.h>`. Userspace also includes that root when it needs to share
an ABI definition, although new musl programs should prefer standard headers
and use TOS syscall headers only for services without a POSIX equivalent.

## Current limits

The musl integration is a compatibility layer rather than a complete Linux
personality. Signals, `clone`-based pthreads, pipes, several filesystem calls,
full terminal semantics, and many socket operations remain unfinished. USB
host-controller and hardware graphics support are also active development
areas. See [`TODO.txt`](TODO.txt) for the current roadmap and the rationale
behind the next milestones.

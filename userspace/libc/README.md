# TOS musl Bootstrap

This directory holds the TOS musl bring-up. The current baseline is musl 1.2.6
configured for a static x86_64 freestanding build with the existing
`x86_64-elf-*` cross toolchain.

Build it from PowerShell with MSYS bash:

```powershell
& 'C:\msys64\usr\bin\bash.exe' userspace/libc/build_tos_musl.sh
```

The script creates `userspace/libc/build-musl-tos`, configures musl with
`--disable-shared`, and builds:

- `lib/libc.a`
- `lib/crt1.o`
- `lib/crti.o`
- `lib/crtn.o`

Link the smoke test:

```powershell
& 'C:\msys64\usr\bin\bash.exe' userspace/libc/link_tos_musl.sh `
    userspace/libc/build-musl-tos/muslhello.elf `
    userspace/libc/tests/hello.c

& 'C:\msys64\usr\bin\bash.exe' userspace/libc/link_tos_musl.sh `
    userspace/libc/build-musl-tos/muslposix.elf `
    userspace/libc/tests/posix_smoke.c
```

When these files exist, `tools/create_disk.sh` packages them under
`/usr/bin`. Runtime smoke tests:

```powershell
python tests/musl_smoke_test.py --timeout 60
python tests/musl_posix_smoke_test.py --timeout 60
```

`tos-musl.mk` exists for one host-build reason: the upstream musl archive rule
passes every object file to `ar` in a single command. On Windows/MSYS that can
hit the process argument length limit. GNU `ar` supports response files, so the
overlay writes the object list to `lib/libc.a.rsp` and invokes `ar` with
`@lib/libc.a.rsp`.

musl is the default libc for userspace. Every program under `bin/` links
`crt1.o` + `libc.a` plus `lib/libtos.a` (the TOS-only half: winman IPC, the
framebuffer, the console, audio, the drawing and font helpers), except:

- `bin/thread` — drives TOS's own `SYS_THREAD_CREATE`/`EXIT`/`JOIN`, while
  musl's pthreads issue Linux `clone(2)`, which the kernel does not implement.
- `games/doom` — a vendored tree that has not been re-ported.
- the PE builds (`*.exe`) — mingw targets, a separate toolchain entirely.

Those still link the hand-rolled `userspace/lib` objects. Everything else
should use standard headers; reach for `<lib/syscall.h>` only for calls musl
has no name for, such as `readdir_path()` or the winman surface.

The kernel now provides enough of the Linux x86_64 syscall ABI for basic static
musl programs to run unchanged:

- process startup stack: `argc`, `argv`, empty `envp`, and an `AT_NULL` auxv
- TLS setup via `arch_prctl(ARCH_SET_FS)`
- stdio/file basics: `read`, `readv`, `write`, `writev`, `open`, `close`,
  `lseek`, `stat`, `fstat`, `newfstatat`, and `getdents64`
- memory basics: `mmap`, `mprotect`, `munmap`, with `brk` explicitly
  unsupported so musl falls back to `mmap`
- process/time probes used by libc: `getpid`, `exit_group`,
  `set_tid_address`, `clock_gettime`, `gettimeofday`, `nanosleep`, `poll`,
  `fcntl`, and tty-shaped `ioctl` calls

This is still a bootstrap layer, not a full Linux personality. The important
known gaps before large ports like Vim are broader terminal behavior, more
filesystem calls (`access`, `readlink`, `dup`, `pipe`, etc. as needed), signals,
and full pthread support (`clone`, signal masks, robust futex handling). The
current strategy is to keep musl mostly upstream and add compatible kernel
syscall semantics where ports naturally hit them.

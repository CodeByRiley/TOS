# TOS userspace libraries

The userspace library layer keeps applications above the raw syscall and IPC
protocols. The intended dependency direction is:

```text
application -> app -> wm -> event -> syscall
            -> ui  -> gfx
            -> process -----------> syscall
```

## Application and window events

`event.h` owns a process-local IPC inbox. Filtered waits preserve unrelated
messages instead of discarding them, which is important when one program uses
the window manager and its own IPC protocol. It is a single-consumer API: after
using `libevent` or `libwm`, receive all IPC through `event_poll*` rather than
calling `ipc_recv` directly.

`wm.h` provides the window-manager protocol. `app.h` adds the common lifecycle
for a single-window program: create a window, bind its shared framebuffer,
rebind it after resize, invalidate it after drawing, and destroy it on exit.
Programs with unusual rendering loops can call `app_poll_event` themselves;
simple programs can use `app_run` and callbacks.

## Processes

`process.h` centralizes executable lookup and process inspection. It supports
colon-separated search paths and the TOS `.elf`/`.exe` naming convention, then
wraps spawn, synchronous exec, snapshots, liveness checks, and bounded waits.

`process_wait` reports that a PID has disappeared or reached zombie/dead state.
It cannot return an exit code yet because the kernel process-list ABI does not
expose one. A future `waitpid` syscall can extend this API without making every
application reimplement the polling logic.

## Rendering helpers

`gfx.h` contains pixel and surface primitives, while `ui.h` provides the
immediate-mode widgets used by GUI programs. `damage.h` tracks and merges dirty
rectangles, and `page_alloc.h` owns reusable page-rounded backing allocations.

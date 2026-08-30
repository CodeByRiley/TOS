# Winman source layout

Winman is one process built from small, independently compiled modules:

- `winman.c` owns the event loop and IPC/input dispatch.
- `desktop.c` discovers launchers and loads desktop artwork.
- `render.c` composes and presents the desktop, chrome, taskbar, and cursor.
- `console.c` owns TTY-backed console windows and font rendering.
- `window.c` owns geometry, hit testing, resizing, stacking, and lifecycle.
- `prompt.c` implements modal prompts.
- `state.c` defines shared process state and fallback artwork once.

`winman.h` and `winman_prototypes.h` are private implementation headers. GUI
applications continue to use `lib/wm.h`; the split does not change the IPC
protocol or expose Winman internals as a public API.

Two mechanisms that are useful outside the window manager live in `lib/`:
`damage` accumulates clipped repaint rectangles, and `page_alloc` creates
zeroed page-aligned buffers suitable for shared memory.

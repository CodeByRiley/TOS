#!/usr/bin/env python3
# Generates build/compile_commands.json for clangd.
#
# No bear on Windows (LD_PRELOAD can't hook .exe), so re-derive the compile
# lines from the same flag sets the Makefiles use. clangd allows exactly one
# entry per source file, so where a source builds several ways (lib/foo.o
# legacy, lib/foo.tos.o musl, lib/foo.pe.o mingw) this picks the variant the
# file is actually edited against: musl for bin/ + libtos objects, legacy
# USER_CFLAGS for the rest of lib/, cross-elf for kernel/. .pe.o-only sources
# are skipped.
#
# Run with a WINDOWS python (so the json gets Windows paths) from anywhere:
#   python tools/gen_compile_commands.py
# Re-run after adding/renaming sources or changing flags in the Makefiles.

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
US = ROOT / "userspace"
MUSL_SRC = US / "libc/musl-1.2.6"
MUSL_BUILD = US / "libc/build-musl-tos"

if not (MUSL_BUILD / "obj/include/bits/alltypes.h").exists():
    sys.exit("musl build tree missing generated headers;\n"
             "run: sh libc/build_tos_musl.sh   (then re-run this)")

# --- flag sets, kept in sync with the Makefiles --------------------------
# Deliberately NO -nostdinc and NO cross-gcc -isystem: clangd injects its own
# builtin headers (stddef.h, stdarg.h, ...) and gcc's copies just conflict.

MUSL_INC = [
    "-I" + str(MUSL_BUILD / "obj/include"),  # generated alltypes.h/syscall.h
    "-I" + str(MUSL_SRC / "arch/x86_64"),
    "-I" + str(MUSL_SRC / "arch/generic"),
    "-I" + str(MUSL_SRC / "include"),
]
USER_INCS = [
    "-I" + str(US), "-I" + str(US / "lib"), "-I" + str(US / "include"),
    "-I" + str(ROOT / "kernel"),
]

KERNEL_FLAGS = [
    "x86_64-elf-gcc", "-c",
    "-std=gnu23", "-ffreestanding", "-mno-red-zone", "-mcmodel=kernel",
    "-fno-pic", "-fno-pie",
    "--target=x86_64-unknown-none-elf",  # keeps _WIN32/__linux__ out of parsing
    "-I" + str(ROOT / "kernel"),
    *MUSL_INC,  # freestanding fallback for <stdint.h> & co.
]
MUSL_FLAGS = [
    "x86_64-elf-gcc", "-c",
    "-std=gnu11", "-ffreestanding", "-fno-pie", "-mno-red-zone",
    "-Wall", "-Wextra", "-DTOS_USE_MUSL", *MUSL_INC, *USER_INCS,
]
USER_FLAGS = [  # USER_CFLAGS: hand-rolled libc
    "x86_64-elf-gcc", "-c",
    "-std=gnu23", "-ffreestanding", "-fno-pie", "-mno-red-zone",
    "-Wall", "-Wextra", *USER_INCS,
]
DOOM_FLAGS = [
    "x86_64-elf-gcc", "-c",
    "-std=gnu17", "-ffreestanding", "-fno-pie", "-mno-red-zone",
    "-DNORMALUNIX", "-DLINUX", *USER_INCS,
    "-I" + str(US / "games/doom/doomgeneric"),
]
HOST_FLAGS = ["gcc", "-c", "-std=c11", "-Wall", "-Wextra",
              "-I" + str(ROOT / "kernel"), *USER_INCS]
_msys = Path("C:/msys64/usr/include")
if _msys.exists():  # stdio.h & friends for host tests/tools
    HOST_FLAGS += ["-isystem", str(_msys)]

entries = {}

def add(src, directory, flags, extra=()):
    src = Path(src)
    if not src.exists():
        return  # generated-not-yet-built, pe-only, etc.
    key = str(src)
    if key not in entries:  # first variant wins
        entries[key] = {"directory": str(directory), "file": key,
                        "arguments": [*flags, *extra]}

def rglob(base):
    return sorted(base.rglob("*.c")) if base.is_dir() else []

# --- kernel ---------------------------------------------------------------
for f in rglob(ROOT / "kernel"):
    if "fs_old" in f.relative_to(ROOT / "kernel").parts:
        continue
    add(f, ROOT, KERNEL_FLAGS)
add(ROOT / "build/generated/symtab.c", ROOT, KERNEL_FLAGS)

# --- userspace ------------------------------------------------------------
# Keep in sync with LIBTOS_SRCS / SIMPLE_BINS in userspace/Makefile.
LIBTOS_SRCS = {
    "lib/syscall.c", "lib/event.c", "lib/wm.c", "lib/app.c",
    "lib/process.c", "lib/gfx.c", "lib/ui.c", "lib/ttf.c", "lib/bmp.c",
    "lib/damage.c", "lib/page_alloc.c", "lib/keymap.c", "lib/console.c",
    "lib/time_stub.c",
}
SIMPLE_BINS = {"thread"}

for f in rglob(US / "lib"):
    rel = f.relative_to(US).as_posix()
    add(f, US, MUSL_FLAGS if rel in LIBTOS_SRCS else USER_FLAGS)

for f in rglob(US / "bin"):
    rel = f.relative_to(US).as_posix()
    if rel.startswith("bin/netsurf/"):
        continue  # handled below
    if rel.startswith("bin/holyd/"):
        # bin/holyd is a submodule, so its sources sit under src/ and its
        # headers are included relative to that (parser/parser.h and so on).
        # ffi_win32.c and the vendor/ copy of lib/ belong to that repository's
        # own Windows build, not to this one, so clangd should not see them
        # under these flags.
        if not rel.startswith("bin/holyd/src/"):
            continue
        if rel == "bin/holyd/src/ffi_win32.c":
            continue
        add(f, US, MUSL_FLAGS, ["-I" + str(US / "bin/holyd/src")])
    elif rel.split("/")[1] in SIMPLE_BINS:
        add(f, US, USER_FLAGS)
    else:
        add(f, US, MUSL_FLAGS)

for f in rglob(US / "games/doom"):
    add(f, US, DOOM_FLAGS)

# --- netsurf (delete this section if you never edit the vendored tree) ----
NS = US / "bin/netsurf"
NETSURF_FLAGS = MUSL_FLAGS + [
    "-D_POSIX_C_SOURCE=200809L", "-D_DEFAULT_SOURCE",
    "-D_ALIGNED=__attribute__((aligned))", "-DSTMTEXHR=1" if False else "-DSTMTEXPR=1",
    "-Dnsframebuffer", "-Dsmall",
    '-DNETSURF_HOMEPAGE="about:welcome"', "-DNETSURF_LOG_LEVEL=INFO",
    '-DNETSURF_BUILTIN_LOG_FILTER="level:WARNING"',
    '-DNETSURF_BUILTIN_VERBOSE_FILTER="level:VERBOSE"',
    '-DNETSURF_FB_RESPATH="/res/netsurf"', '-DNETSURF_FB_FONTPATH=""',
    "-I" + str(US / "netsurf_compat"), "-I" + str(US / "netsurf_compat/generated"),
    "-I" + str(NS / "netsurf/include"), "-I" + str(NS / "netsurf/content/handlers"),
    "-I" + str(NS / "netsurf/frontends"),
] + ["-I" + str(NS / lib / "include") for lib in (
    "libcss", "libdom", "libhubbub", "libnsfb", "libnsutils", "libnslog",
    "libparserutils", "libwapcaplet", "libutf8proc", "libnsgif",
    "libnsbmp", "libnspsl", "libsvgtiny")]

# Per-object private flags, from the target-specific vars in userspace/Makefile.
NETSURF_PRIVATE = [
    ("bin/netsurf/libcss/",         [f"-I{NS}/libcss/src"]),
    ("bin/netsurf/libdom/",         [f"-I{NS}/libdom", f"-I{NS}/libdom/src"]),
    ("bin/netsurf/libhubbub/",      [f"-I{NS}/libhubbub/src"]),
    ("bin/netsurf/libnsfb/",        [f"-I{NS}/libnsfb/src", f"-I{NS}/netsurf"]),
    ("bin/netsurf/libnsutils/",     [f"-I{NS}/libnsutils/src"]),
    ("bin/netsurf/libnslog/",       [f"-I{NS}/libnslog/src"]),
    ("bin/netsurf/libparserutils/", [f"-I{NS}/libparserutils/src"]),
    ("bin/netsurf/libwapcaplet/",   [f"-I{NS}/libwapcaplet/src"]),
    ("bin/netsurf/libutf8proc/",    [f"-I{NS}/libutf8proc/src"]),
    ("bin/netsurf/libnsgif/",       [f"-I{NS}/libnsgif/src"]),
    ("bin/netsurf/libnsbmp/",       [f"-I{NS}/libnsbmp/src"]),
    ("bin/netsurf/libnspsl/",       [f"-I{NS}/libnspsl/src"]),
    ("bin/netsurf/libsvgtiny/",     [f"-I{NS}/libsvgtiny/src"]),
    ("bin/netsurf/netsurf/",        [f"-I{NS}/netsurf"]),
    ("netsurf_compat/tos_surface",  [f"-I{NS}/libnsfb/src", f"-I{NS}/netsurf"]),
    ("netsurf_compat/messages",     [f"-I{NS}/netsurf"]),
    ("netsurf_compat/generated/framebuffer-gui", [f"-I{NS}/netsurf"]),
    ("netsurf_compat/generated/fbtk-text",
     [f"-I{NS}/netsurf/frontends/framebuffer/fbtk", f"-I{NS}/netsurf"]),
    ("bin/netsurf/netsurf/utils/messages.c",
     ["-Dmessages_add_from_file=netsurf_messages_add_from_file"]),
]

NS_DIRS = ["libwapcaplet/src", "libparserutils/src", "libcss/src",
           "libhubbub/src", "libdom/src", "libdom/bindings/hubbub",
           "libnsutils/src", "netsurf/content", "netsurf/utils",
           "netsurf/desktop", "netsurf/frontends/framebuffer", "libnsfb/src"]
NS_EXCLUDE = ("/content/fetchers/curl.c", "/content/fs_backing_store.c",
              "/content/handlers/image/", "/javascript/content.c",
              "/frontends/framebuffer/fb_search.c",
              "/frontends/framebuffer/gui.c",
              "/frontends/framebuffer/fbtk/text.c",
              "/frontends/framebuffer/font_freetype.c")

ns_files = set()
for d in NS_DIRS:
    for f in (NS / d).rglob("*.c"):
        if f.parent.name in {"test", "duktape"}:
            continue
        if f.name in {"css_property_parser_gen.c", "autogenerated-element-type.c"}:
            continue
        if any(x in "/" + f.relative_to(US).as_posix() for x in NS_EXCLUDE):
            continue
        ns_files.add(f)
for f in (US / "netsurf_compat").glob("*.c"):
    ns_files.add(f)
for f in (US / "netsurf_compat/generated").glob("*.c"):
    ns_files.add(f)

for f in sorted(ns_files):
    rel = f.relative_to(US).as_posix()
    extra = [x for p, xs in NETSURF_PRIVATE if rel.startswith(p) for x in xs]
    add(f, US, NETSURF_FLAGS, extra)

# --- host tests + tools ----------------------------------------------------
for f in rglob(ROOT / "tests") + rglob(ROOT / "tools"):
    add(f, ROOT, HOST_FLAGS)

# ---------------------------------------------------------------------------
out = ROOT / "build"
out.mkdir(exist_ok=True)
(out / "compile_commands.json").write_text(json.dumps(list(entries.values()), indent=1))
print(f"wrote build/compile_commands.json, {len(entries)} entries")

#SHELL := /usr/bin/bash
#SHELL := C:/msys64/usr/bin/bash.exe

USER_ELF_CC  := x86_64-elf-gcc
USER_ELF_CXX := x86_64-elf-g++
USER_ELF_LD  := x86_64-elf-ld

#USER_ELF_CC  := /c/elf-tools/bin/x86_64-elf-gcc
#USER_ELF_LD  := /c/elf-tools/bin/x86_64-elf-ld

USER_PE_CC   := x86_64-w64-mingw32-gcc
USER_AS      := nasm
USER_CFLAGS  := -ffreestanding -nostdlib -fno-pie -mno-red-zone -static \
                  -I . -I lib -I include -I ../kernel -O2 -Wall -Wextra -std=gnu23 \
                  -MMD -MP -DDEBUG
USER_LDFLAGS := -nostdlib -static -T lib/user.ld
BUILD_DOOM ?= 0
SHOW_DOOM_INFO ?= 0
BUILD_NETSURF ?= 0

# PE binaries cannot link ELF objects, so every object is built twice: once
# by the ELF cross-compiler into foo.o, once by mingw into foo.pe.o. The
# SOURCES are shared , bin/hello/hello.c produces both hello.elf and
# hello.exe , because the syscall layer is typed on sysarg_t rather than
# `long`, which differs in width between the two toolchains. See the note
# in lib/syscall.h.
#
# -fno-asynchronous-unwind-tables drops .pdata/.xdata: nothing unwinds, and
# it keeps the section count down to what the loader has to walk.
PE_CFLAGS := -ffreestanding -nostdlib -fno-pie -mno-red-zone -static \
							-fno-asynchronous-unwind-tables \
              -I . -I lib -I include -I ../kernel -O2 -Wall -Wextra -std=gnu23 -DDEBUG

# mingw counterparts of crt0.s and syscall.s; everything else is the same
# source the ELF build uses.
PE_LIB_OBJS := lib/crt0_pe.pe.o lib/syscall_pe.pe.o lib/syscall.pe.o \
               lib/syscall_posix.pe.o \
               lib/printf.pe.o lib/string.pe.o lib/string_extra.pe.o \
               lib/malloc.pe.o lib/pthread.pe.o

# C-source PE lib objects (crt0_pe is assembly and has its own rule).
PE_LIB_C_OBJS := $(filter-out lib/crt0_pe.pe.o, $(PE_LIB_OBJS))

# Self-contained PE programs: their own entry point and stubs, no PE_LIB.
PE_BINS := pe_test
PE_BIN_OBJS := $(foreach b,$(PE_BINS),bin/$(b)/$(b).pe.o)
PE_BIN_EXES := $(foreach b,$(PE_BINS),bin/$(b)/$(b).exe)

# PE variants built from the very same sources as the ELF binaries of the
# same name. bin/hello/hello.c -> hello.elf and hello.exe.
PE_VARIANTS := hello ls
PE_VARIANT_OBJS := $(foreach b,$(PE_VARIANTS),bin/$(b)/$(b).pe.o)
PE_VARIANT_EXES := $(foreach b,$(PE_VARIANTS),bin/$(b)/$(b).exe)

# The hand-rolled libc. musl is the default now (see MUSL_BINS), so the only
# things still linking this are thread.elf , which drives TOS's own thread
# syscalls rather than clone(2) , and DOOM, which is a vendored tree nobody
# has re-ported. Do not add binaries here; add them to MUSL_BINS.
#
# string_extra carries strstr/strcpy/strcat/strrchr/strcasecmp, which
# string.c does not define. Without it any tool calling one of those only
# links while the call is optimised away.
# stat.o is in the base set, not the DOOM-only extras: stat/fstat are real
# syscalls now, so every binary should be able to call them.
# time_stub carries time()/clock_gettime()/time_to_calendar(). It used to be
# DOOM-only, which meant any other binary calling time() failed to link.
LIB_OBJS := lib/crt0.o lib/syscall_asm.o lib/syscall.o lib/syscall_posix.o lib/event.o lib/wm.o lib/app.o lib/process.o \
            lib/printf.o lib/malloc.o lib/stdio.o lib/stdio_extra.o lib/string.o lib/keymap.o lib/console.o \
            lib/stdlib_extra.o lib/string_extra.o lib/stat.o lib/bmp.o lib/gfx.o lib/ui.o \
            lib/damage.o lib/page_alloc.o lib/pthread.o lib/time_stub.o

TTF_OBJS := lib/ttf.o

# C-source lib objects (.s sources have their own explicit rules below).
LIB_C_OBJS := $(filter-out lib/crt0.o lib/syscall_asm.o, $(LIB_OBJS)) $(TTF_OBJS)

# One binary per directory under bin/, named after the directory.
# Doom is special (multi-source) and gets its own rules.
#
# musl is the default libc: everything below links crt1/libc.a plus libtos.a
# rather than crt0.o + LIB_OBJS, and reaches POSIX through the real headers.
# The list is explicit rather than a wildcard so adding a binary is a
# deliberate act; a name here must NOT also appear in SIMPLE_BINS.
#
# The rule below builds one .c into one .elf. Programs with several
# translation units have explicit rules below (Winman and HolyD).
MUSL_DIR      := libc/build-musl-tos
MUSL_SRC_DIR  := libc/musl-1.2.6
MUSL_LINK     := libc/link_tos_musl.sh
MUSL_LINKER_SCRIPT := lib/user.ld
MUSL_LIBC     := $(MUSL_DIR)/lib/libc.a
MUSL_BINS     := cat plist hello ls pkill gfx btop uidemo deskelf notepad tree \
                 mount umount lsdisk shutdown reboot fdchild mmaptest audiotest \
                 diskinfo sync wipe mkfs stress stress_peer mtest vmtest muse sh winman netmon ping udpecho
MUSL_BIN_ELFS := $(foreach b,$(MUSL_BINS),bin/$(b)/$(b).elf)

# Winman is intentionally a multi-source program. Its modules share one
# private header but compile independently, so a prompt change does not
# rebuild the compositor, console, desktop, and event loop together.
WINMAN_SRCS := bin/winman/winman.c bin/winman/state.c bin/winman/desktop.c \
               bin/winman/render.c bin/winman/console.c \
               bin/winman/window.c bin/winman/prompt.c
WINMAN_OBJS := $(WINMAN_SRCS:.c=.tos.o)
WINMAN_ELF  := bin/winman/winman.elf

# libtos.a , the parts of lib/ that musl does not provide: winman IPC, the
# framebuffer, the console, audio, TOS-only syscalls, and the drawing and
# font helpers. Everything here is TOS-specific and collides with nothing in
# libc.a; the POSIX wrappers that did collide live in syscall_posix.c, which
# a musl binary never links.
#
# Built from the same sources as the legacy objects but against musl's
# headers, so a .tos.o is not interchangeable with a .o , same pattern as
# the .pe.o variants above.
LIBTOS_SRCS := lib/syscall.c lib/event.c lib/wm.c lib/app.c lib/process.c \
               lib/gfx.c lib/ui.c lib/ttf.c \
               lib/bmp.c lib/damage.c lib/page_alloc.c lib/keymap.c \
               lib/console.c lib/time_stub.c
LIBTOS_COBJS := $(LIBTOS_SRCS:.c=.tos.o)
# syscall_asm.o is assembly and identical either way , reuse it rather than
# assembling a second copy.
LIBTOS_OBJS  := $(LIBTOS_COBJS) lib/syscall_asm.o
LIBTOS      := lib/libtos.a

# musl's headers must come before TOS's so <stdio.h> and friends resolve to
# musl. -nostdinc drops the host's entirely; the compiler's own include dir
# is added back for stddef.h / stdarg.h.
MUSL_INC    := -nostdinc \
               -I $(MUSL_DIR)/obj/include \
               -I $(MUSL_SRC_DIR)/arch/x86_64 \
               -I $(MUSL_SRC_DIR)/arch/generic \
               -I $(MUSL_SRC_DIR)/include \
               -isystem $(shell $(USER_ELF_CC) -print-file-name=include)
MUSL_CFLAGS := -ffreestanding -nostdlib -fno-pie -mno-red-zone -static \
               -O2 -Wall -Wextra -std=gnu11 -MMD -MP \
               $(MUSL_INC) -I . -I lib -I include -I ../kernel \
               -DTOS_USE_MUSL -DDEBUG

# thread.elf stays on the hand-rolled lib: it drives TOS's own thread
# syscalls (SYS_THREAD_CREATE/EXIT/JOIN), and musl's pthreads issue
# Linux clone(2), which the kernel does not implement.
SIMPLE_BINS := thread

SIMPLE_BIN_OBJS := $(foreach b,$(SIMPLE_BINS),bin/$(b)/$(b).o)
SIMPLE_BIN_ELFS := $(foreach b,$(SIMPLE_BINS),bin/$(b)/$(b).elf)

# Simple binaries that render text through lib/ttf.o. They link the base set
# plus the rasteriser; see the link rules below.
TTF_BINS :=
TTF_BIN_ELFS := $(foreach b,$(TTF_BINS),bin/$(b)/$(b).elf)

# bin/holyd is a submodule , github.com/CodeByRiley/HolyD , so the sources
# sit under its src/ and the rest of that repository (its own Makefile,
# README, samples, and its standalone graphics fallbacks) is not ours to
# compile.
HOLYD_SRC_DIR := bin/holyd/src

# A clone without --recursive leaves bin/holyd empty. Say so, rather than
# letting the wildcards below come back empty and produce either a link of
# nothing or a baffling "no rule to make target". Skipped for clean, which
# has no reason to need the sources.
ifeq ($(filter clean,$(MAKECMDGOALS)),)
ifeq ($(wildcard $(HOLYD_SRC_DIR)/main.c),)
$(error HolyD submodule is not checked out. Run: git submodule update --init)
endif
endif
# ffi.c is portable and calls the ffi_platform.h host; exactly one host is
# linked. ffi_win32.c is the one for the standalone Windows build and needs
# windows.h, so the wildcard has to drop it rather than pick it up here.
HOLYD_SOURCES := $(filter-out $(HOLYD_SRC_DIR)/ffi_win32.c, \
                   $(wildcard $(HOLYD_SRC_DIR)/*.c)) \
                 $(wildcard $(HOLYD_SRC_DIR)/lexer/*.c) \
                 $(wildcard $(HOLYD_SRC_DIR)/parser/*.c) \
                 $(wildcard $(HOLYD_SRC_DIR)/ast/*.c)
# holyd is the one musl program built from more than one translation unit,
# so it compiles to .tos.o objects of its own rather than going through the
# one-source-per-binary MUSL_BIN rule below.
HOLYD_OBJS := $(HOLYD_SOURCES:.c=.tos.o)
# The platform adapters in HolyD must choose TOS's public gfx/BMP/input/font
# interfaces. Keep this explicit instead of detecting TOS from include paths.
HOLYD_CFLAGS := $(MUSL_CFLAGS) -I $(HOLYD_SRC_DIR) \
                 -DHOLYD_TARGET_TOS=1 -MMD -MP

DOOM_SRC_DIR := games/doom/doomgeneric
DOOM_SOURCES := $(filter-out \
                $(DOOM_SRC_DIR)/doomgeneric_allegro.c \
                $(DOOM_SRC_DIR)/doomgeneric_emscripten.c \
                $(DOOM_SRC_DIR)/doomgeneric_linuxvt.c \
                $(DOOM_SRC_DIR)/doomgeneric_sdl.c \
                $(DOOM_SRC_DIR)/doomgeneric_soso.c \
                $(DOOM_SRC_DIR)/doomgeneric_sosox.c \
                $(DOOM_SRC_DIR)/doomgeneric_win.c \
                $(DOOM_SRC_DIR)/doomgeneric_xlib.c \
                $(DOOM_SRC_DIR)/i_allegromusic.c \
                $(DOOM_SRC_DIR)/i_allegrosound.c \
                $(DOOM_SRC_DIR)/i_sdlmusic.c \
                $(DOOM_SRC_DIR)/i_sdlsound.c \
                $(DOOM_SRC_DIR)/i_cdmus.c \
                $(DOOM_SRC_DIR)/i_joystick.c, \
                $(wildcard $(DOOM_SRC_DIR)/*.c))

DOOM_OBJS := $(DOOM_SOURCES:.c=.o)

DOOM_PLATFORM_SRC := games/doom/doomgeneric_kernel.c games/doom/joystick_stub.c
DOOM_PLATFORM_OBJS := $(DOOM_PLATFORM_SRC:.c=.o)

DOOM_EXTRA_OBJS := lib/string_extra.o lib/stdio_extra.o \
                   lib/math_stub.o lib/setjmp_stub.o lib/errno.o \
                   lib/unistd_stub.o

DOOM_CFLAGS  := -ffreestanding -nostdlib -fno-pie -mno-red-zone -static \
                  -I . -I include -I lib \
                  -I $(DOOM_SRC_DIR) \
                  -std=gnu17 -O2 -Wno-error \
                  -DNORMALUNIX -DLINUX


bin/doom/doom.elf: $(DOOM_OBJS) $(DOOM_PLATFORM_OBJS) $(LIB_OBJS) $(DOOM_EXTRA_OBJS)
	$(USER_ELF_LD) $(USER_LDFLAGS) $^ -o $@

ifeq ($(SHOW_DOOM_INFO),1)
$(info DOOM_SRC_DIR = $(DOOM_SRC_DIR))
$(info DOOM_SOURCES count = $(words $(DOOM_SOURCES)))
$(info DOOM_OBJS first = $(firstword $(DOOM_OBJS)))
endif

# Generic catch-all for doom sources (and any other .c that lacks an explicit
# rule). Uses DOOM_CFLAGS by design , anything compiled with USER_CFLAGS must
# be in LIB_C_OBJS or SIMPLE_BIN_OBJS so the static patterns below claim it.
%.o: %.c
	$(USER_ELF_CC) $(DOOM_CFLAGS) $(CCDB) -c $< -o $@

USERSPACE_TARGETS := $(SIMPLE_BIN_ELFS) $(MUSL_BIN_ELFS) $(PE_BIN_EXES) $(PE_VARIANT_EXES) bin/holyd.elf
ifeq ($(BUILD_DOOM),1)
USERSPACE_TARGETS += bin/doom/doom.elf
endif
ifeq ($(BUILD_NETSURF),1)
USERSPACE_TARGETS += bin/netsurf/netsurf.elf
endif

.PHONY: all clean doom netsurf
all: $(USERSPACE_TARGETS)

doom: bin/doom/doom.elf
netsurf: bin/netsurf/netsurf.elf

# --- library objects -----------------------------------------------------

lib/crt0.o: lib/crt0.s
	$(USER_AS) -f elf64 $< -o $@

lib/syscall_asm.o: lib/syscall.s
	$(USER_AS) -f elf64 $< -o $@

# Static pattern: every C-source lib object compiles with USER_CFLAGS.
$(LIB_C_OBJS): %.o: %.c
	$(USER_ELF_CC) $(USER_CFLAGS) -c $< -o $@

# --- simple binaries -----------------------------------------------------

# Static pattern for the per-binary .o (USER_CFLAGS, not DOOM_CFLAGS).
$(SIMPLE_BIN_OBJS): %.o: %.c
	$(USER_ELF_CC) $(USER_CFLAGS) -c $< -o $@

# Header dependencies generated by -MMD. Without these a change to Winman's
# private state or module interface can leave stale objects in the image.
# Leading '-' so a clean tree with no .d files yet is not an error.
#
# The musl-linked binaries and libtos objects belong here as much as anything
# else, and for a sharper reason: they include ../kernel/arch/syscall_abi.h,
# which is the ONE definition of every struct crossing a syscall. Single
# sourcing it is only worth anything if both sides rebuild together -- without
# these entries a change to that header rebuilt the kernel and left every
# userspace binary reading the old layout, which does not fail to build, it
# just decodes the reply wrongly at runtime.
#
# link_tos_musl.sh already writes bin/foo/foo.d for each binary it links; those
# were being generated and then ignored.
USER_DEP_FILES := $(patsubst %.o,%.d,$(LIB_C_OBJS) $(SIMPLE_BIN_OBJS) $(HOLYD_OBJS) $(NETSURF_OBJS)) \
                  $(WINMAN_OBJS:.tos.o=.tos.d) \
                  $(LIBTOS_COBJS:.tos.o=.tos.d) \
                  $(MUSL_BIN_ELFS:.elf=.d)
-include $(USER_DEP_FILES)

# Static pattern for the link step. bin/foo/foo.elf <- bin/foo/foo.o + lib.
$(filter-out $(TTF_BIN_ELFS),$(SIMPLE_BIN_ELFS)): bin/%.elf: bin/%.o $(LIB_OBJS)
	$(USER_ELF_LD) $(USER_LDFLAGS) $^ -o $@

# Same link, plus the font rasteriser. ld pulls in whole objects rather than
# archive members, so ttf.o stays off the default line , it is 70 KiB that
# every tool not drawing glyphs would otherwise carry.
$(TTF_BIN_ELFS): bin/%.elf: bin/%.o $(LIB_OBJS) $(TTF_OBJS)
	$(USER_ELF_LD) $(USER_LDFLAGS) $^ -o $@

# --- musl-linked binaries ------------------------------------------------

# link_tos_musl.sh owns the include paths and the crt1/crti/libc.a/crtn
# ordering. Going through it keeps one copy of those flags rather than a
# second set here that could drift.

# musl itself. The build tree is generated rather than committed (see
# libc/.gitignore), so a clean checkout has to produce it before anything
# can compile against it. Without this rule make stops dead at "No rule to
# make target 'libc/build-musl-tos/lib/libc.a'", and building musl becomes
# a manual step that only gets remembered after CI fails.
#
# build_tos_musl.sh configures on the first run only and is safe to
# re-enter. MAKEFLAGS is cleared because the script drives musl's own
# Makefile with a plain `make`, which would otherwise inherit this build's
# jobserver and warn that it cannot reach it.
$(MUSL_LIBC):
	MAKEFLAGS= CC=$(USER_ELF_CC) "$(SHELL)" libc/build_tos_musl.sh

# A .tos.o needs more than the archive. obj/include/bits holds alltypes.h
# and syscall.h, which musl generates at configure time and which have no
# counterpart in the source tree, so compiling against an unbuilt musl
# fails on a missing include rather than on a missing library. Hanging the
# dependency on libc.a covers both, and rebuilds these objects whenever
# musl is rebuilt, which is what a change to those headers should do.
$(LIBTOS_COBJS): %.tos.o: %.c $(MUSL_LIBC)
	$(USER_ELF_CC) $(MUSL_CFLAGS) $(CCDB) -c $< -o $@

$(LIBTOS): $(LIBTOS_OBJS)
	rm -f $@ && x86_64-elf-ar rcs $@ $^

$(filter-out $(WINMAN_ELF),$(MUSL_BIN_ELFS)): bin/%.elf: bin/%.c $(MUSL_LIBC) $(LIBTOS) $(MUSL_LINK) $(MUSL_LINKER_SCRIPT)
	CC=$(USER_ELF_CC) "$(SHELL)" $(MUSL_LINK) $@ $< $(LIBTOS)

$(WINMAN_OBJS): %.tos.o: %.c $(MUSL_LIBC)
	$(USER_ELF_CC) $(MUSL_CFLAGS) $(CCDB) -MMD -MP -c $< -o $@

$(WINMAN_ELF): $(WINMAN_OBJS) $(MUSL_LIBC) $(LIBTOS) $(MUSL_LINK) $(MUSL_LINKER_SCRIPT)
	CC=$(USER_ELF_CC) "$(SHELL)" $(MUSL_LINK) $@ $(WINMAN_OBJS) $(LIBTOS)

# --- holyd rules ---------------------------------------------------------

# HolyD headers use paths rooted at bin/holyd (for example, lexer/lexer.h).
$(HOLYD_OBJS): %.tos.o: %.c $(MUSL_LIBC)
	$(USER_ELF_CC) $(HOLYD_CFLAGS) $(CCDB) -c $< -o $@

# Link holyd.elf. Objects, not a source, so link_tos_musl.sh gets them
# straight through , it passes anything after the output name to the
# compiler driver unchanged.
#
# The binary lands beside the submodule rather than inside it: bin/holyd is
# a checkout of another repository and build output there would show up as
# local changes to it. The .tos.o and .tos.d files still land next to their
# sources, which is fine only because HolyD's .gitignore covers *.o and *.d.
bin/holyd.elf: $(HOLYD_OBJS) $(MUSL_LIBC) $(LIBTOS) $(MUSL_LINK) $(MUSL_LINKER_SCRIPT)
	CC=$(USER_ELF_CC) "$(SHELL)" $(MUSL_LINK) $@ $(HOLYD_OBJS) $(LIBTOS)

# --- netsurf rules ---------------------------------------------------------

NETSURF_SRC_DIR := bin/netsurf

# Use NetSurf's core libraries and framebuffer frontend.
NETSURF_LIBRARY_DIRS := \
	$(NETSURF_SRC_DIR)/libwapcaplet/src \
	$(NETSURF_SRC_DIR)/libparserutils/src \
	$(NETSURF_SRC_DIR)/libcss/src \
	$(NETSURF_SRC_DIR)/libhubbub/src \
	$(NETSURF_SRC_DIR)/libdom/src \
	$(NETSURF_SRC_DIR)/libdom/bindings/hubbub \
	$(NETSURF_SRC_DIR)/libnsutils/src

NETSURF_BROWSER_DIRS := \
	$(NETSURF_SRC_DIR)/netsurf/content \
	$(NETSURF_SRC_DIR)/netsurf/utils \
	$(NETSURF_SRC_DIR)/netsurf/desktop \
	$(NETSURF_SRC_DIR)/netsurf/frontends/framebuffer

# NetSurf's own sources are a separate checkout that a plain clone of TOS does
# not have. `:=` expands $(shell ...) while the Makefile is read, so an
# unguarded find runs on every build and prints a screenful of "No such file or
# directory" even at BUILD_NETSURF=0. Only walk the tree when the port is
# actually being built, and take find from PATH , hardcoding one machine's
# msys64 breaks every other machine. Override NETSURF_FIND if the first find on
# your PATH is the Windows one.
NETSURF_FIND ?= /usr/bin/find
NETSURF_SOURCE_DIRS := $(wildcard $(NETSURF_LIBRARY_DIRS) $(NETSURF_BROWSER_DIRS))

ifeq ($(BUILD_NETSURF),1)
ifeq ($(NETSURF_SOURCE_DIRS),)
$(error BUILD_NETSURF=1 but $(NETSURF_SRC_DIR) holds no NetSurf sources , see README)
endif
NETSURF_SOURCES := $(shell $(NETSURF_FIND) \
	$(NETSURF_SOURCE_DIRS) \
	-type d \\( -name test -o -name duktape \\) -prune \
	-o -type f -name \\*.c \
	-not -name css_property_parser_gen.c \
	-not -name autogenerated-element-type.c \
	-not -path \\*/content/fetchers/curl.c \
	-not -path \\*/content/fs_backing_store.c \
	-not -path \\*/content/handlers/image/bmp.c \
	-not -path \\*/content/handlers/image/gif.c \
	-not -path \\*/content/handlers/image/ico.c \
	-not -path \\*/content/handlers/image/jpeg.c \
	-not -path \\*/content/handlers/image/jpegxl.c \
	-not -path \\*/content/handlers/image/nssprite.c \
	-not -path \\*/content/handlers/image/png.c \
	-not -path \\*/content/handlers/image/rsvg.c \
	-not -path \\*/content/handlers/image/rsvg246.c \
	-not -path \\*/content/handlers/image/svg.c \
	-not -path \\*/content/handlers/image/video.c \
	-not -path \\*/content/handlers/image/webp.c \
	-not -path \\*/content/handlers/javascript/content.c \
	-not -path '*/frontends/framebuffer/fb_search.c' \
	-not -path '*/frontends/framebuffer/gui.c' \
	-not -path '*/frontends/framebuffer/fbtk/text.c' \
	-not -path '*/frontends/framebuffer/font_freetype.c' \
	-print)
endif

NETSURF_NSFB_SOURCES := \
	$(NETSURF_SRC_DIR)/libnsfb/src/libnsfb.c \
	$(NETSURF_SRC_DIR)/libnsfb/src/dump.c \
	$(NETSURF_SRC_DIR)/libnsfb/src/cursor.c \
	$(NETSURF_SRC_DIR)/libnsfb/src/palette.c \
	$(NETSURF_SRC_DIR)/libnsfb/src/plot/api.c \
	$(NETSURF_SRC_DIR)/libnsfb/src/plot/util.c \
	$(NETSURF_SRC_DIR)/libnsfb/src/plot/generic.c \
	$(NETSURF_SRC_DIR)/libnsfb/src/plot/32bpp-xrgb8888.c \
	$(NETSURF_SRC_DIR)/libnsfb/src/plot/32bpp-xbgr8888.c \
	$(NETSURF_SRC_DIR)/libnsfb/src/plot/16bpp.c \
	$(NETSURF_SRC_DIR)/libnsfb/src/plot/8bpp.c \
	$(NETSURF_SRC_DIR)/libnsfb/src/surface/surface.c

NETSURF_SOURCES += $(NETSURF_NSFB_SOURCES)
NETSURF_SOURCES += netsurf_compat/zlib.c
NETSURF_SOURCES += netsurf_compat/messages.c
NETSURF_SOURCES += netsurf_compat/tos_surface.c
NETSURF_SOURCES += $(NETSURF_SRC_DIR)/libnsfb/src/surface/ram.c
NETSURF_SOURCES += netsurf_compat/generated/font-ns-sans.c
NETSURF_GUI := netsurf_compat/generated/framebuffer-gui.c
NETSURF_FBTK_TEXT := netsurf_compat/generated/fbtk-text.c
NETSURF_SOURCES += $(NETSURF_GUI) $(NETSURF_FBTK_TEXT)

NETSURF_IMAGE_NAMES := \
	left_arrow right_arrow reload stop_image history_image \
	left_arrow_g right_arrow_g reload_g stop_image_g history_image_g \
	scrolll scrollr scrollu scrolld osk_image \
	pointer_image hand_image caret_image menu_image progress_image move_image \
	throbber0 throbber1 throbber2 throbber3 throbber4 \
	throbber5 throbber6 throbber7 throbber8

NETSURF_IMAGE_SOURCES := $(addprefix netsurf_compat/generated/image-,\
	$(addsuffix .c,$(NETSURF_IMAGE_NAMES)))
NETSURF_SOURCES += $(NETSURF_IMAGE_SOURCES)

NETSURF_OBJS := $(NETSURF_SOURCES:.c=.tos.o)

# Share public headers, then add each component's private headers below.
NETSURF_CFLAGS := $(MUSL_CFLAGS) \
                  -D_POSIX_C_SOURCE=200809L \
                  -D_DEFAULT_SOURCE \
                  -D_ALIGNED="__attribute__((aligned))" \
                  -DSTMTEXPR=1 \
                  -Dnsframebuffer \
                  -Dsmall \
                  -DNETSURF_HOMEPAGE=\"about:welcome\" \
                  -DNETSURF_LOG_LEVEL=INFO \
                  -DNETSURF_BUILTIN_LOG_FILTER=\"level:WARNING\" \
                  -DNETSURF_BUILTIN_VERBOSE_FILTER=\"level:VERBOSE\" \
                  -DNETSURF_FB_RESPATH=\"/res/netsurf\" \
                  -DNETSURF_FB_FONTPATH=\"\" \
                  -I netsurf_compat \
                  -I netsurf_compat/generated \
                  -I $(NETSURF_SRC_DIR) \
                  -I $(NETSURF_SRC_DIR)/netsurf/include \
                  -I $(NETSURF_SRC_DIR)/netsurf/content/handlers \
                  -I $(NETSURF_SRC_DIR)/netsurf/frontends \
                  -I $(NETSURF_SRC_DIR)/libcss/include \
                  -I $(NETSURF_SRC_DIR)/libdom/include \
                  -I $(NETSURF_SRC_DIR)/libhubbub/include \
                  -I $(NETSURF_SRC_DIR)/libnsfb/include \
                  -I $(NETSURF_SRC_DIR)/libnsutils/include \
                  -I $(NETSURF_SRC_DIR)/libnslog/include \
                  -I $(NETSURF_SRC_DIR)/libparserutils/include \
                  -I $(NETSURF_SRC_DIR)/libwapcaplet/include \
                  -I $(NETSURF_SRC_DIR)/libutf8proc/include \
                  -I $(NETSURF_SRC_DIR)/libnsgif/include \
                  -I $(NETSURF_SRC_DIR)/libnsbmp/include \
                  -I $(NETSURF_SRC_DIR)/libnspsl/include \
                  -I $(NETSURF_SRC_DIR)/libsvgtiny/include \
                  -MMD -MP

$(filter $(NETSURF_SRC_DIR)/netsurf/%,$(NETSURF_OBJS)): \
	NETSURF_PRIVATE_CFLAGS := -I $(NETSURF_SRC_DIR)/netsurf
$(filter $(NETSURF_SRC_DIR)/libcss/%,$(NETSURF_OBJS)): \
	NETSURF_PRIVATE_CFLAGS := -I $(NETSURF_SRC_DIR)/libcss/src
$(filter $(NETSURF_SRC_DIR)/libdom/%,$(NETSURF_OBJS)): \
	NETSURF_PRIVATE_CFLAGS := -I $(NETSURF_SRC_DIR)/libdom -I $(NETSURF_SRC_DIR)/libdom/src
$(filter $(NETSURF_SRC_DIR)/libhubbub/%,$(NETSURF_OBJS)): \
	NETSURF_PRIVATE_CFLAGS := -I $(NETSURF_SRC_DIR)/libhubbub/src
$(filter $(NETSURF_SRC_DIR)/libnsfb/%,$(NETSURF_OBJS)): \
	NETSURF_PRIVATE_CFLAGS := -I $(NETSURF_SRC_DIR)/libnsfb/src -I $(NETSURF_SRC_DIR)/netsurf
netsurf_compat/tos_surface.tos.o: \
	NETSURF_PRIVATE_CFLAGS := -I $(NETSURF_SRC_DIR)/libnsfb/src -I $(NETSURF_SRC_DIR)/netsurf
netsurf_compat/messages.tos.o: \
	NETSURF_PRIVATE_CFLAGS := -I $(NETSURF_SRC_DIR)/netsurf
netsurf_compat/generated/framebuffer-gui.tos.o: \
	NETSURF_PRIVATE_CFLAGS := -I $(NETSURF_SRC_DIR)/netsurf
netsurf_compat/generated/fbtk-text.tos.o: \
	NETSURF_PRIVATE_CFLAGS := -I $(NETSURF_SRC_DIR)/netsurf/frontends/framebuffer/fbtk -I $(NETSURF_SRC_DIR)/netsurf
bin/netsurf/netsurf/utils/messages.tos.o: \
	NETSURF_PRIVATE_CFLAGS += -Dmessages_add_from_file=netsurf_messages_add_from_file
bin/netsurf/netsurf/content/fetchers/file/file.tos.o: \
	netsurf_compat/utils/config.h
$(filter $(NETSURF_SRC_DIR)/libnsutils/%,$(NETSURF_OBJS)): \
	NETSURF_PRIVATE_CFLAGS := -I $(NETSURF_SRC_DIR)/libnsutils/src
$(filter $(NETSURF_SRC_DIR)/libnslog/%,$(NETSURF_OBJS)): \
	NETSURF_PRIVATE_CFLAGS := -I $(NETSURF_SRC_DIR)/libnslog/src
$(filter $(NETSURF_SRC_DIR)/libparserutils/%,$(NETSURF_OBJS)): \
	NETSURF_PRIVATE_CFLAGS := -I $(NETSURF_SRC_DIR)/libparserutils/src
$(filter $(NETSURF_SRC_DIR)/libwapcaplet/%,$(NETSURF_OBJS)): \
	NETSURF_PRIVATE_CFLAGS := -I $(NETSURF_SRC_DIR)/libwapcaplet/src
$(filter $(NETSURF_SRC_DIR)/libutf8proc/%,$(NETSURF_OBJS)): \
	NETSURF_PRIVATE_CFLAGS := -I $(NETSURF_SRC_DIR)/libutf8proc/src
$(filter $(NETSURF_SRC_DIR)/libnsgif/%,$(NETSURF_OBJS)): \
	NETSURF_PRIVATE_CFLAGS := -I $(NETSURF_SRC_DIR)/libnsgif/src
$(filter $(NETSURF_SRC_DIR)/libnsbmp/%,$(NETSURF_OBJS)): \
	NETSURF_PRIVATE_CFLAGS := -I $(NETSURF_SRC_DIR)/libnsbmp/src
$(filter $(NETSURF_SRC_DIR)/libnspsl/%,$(NETSURF_OBJS)): \
	NETSURF_PRIVATE_CFLAGS := -I $(NETSURF_SRC_DIR)/libnspsl/src
$(filter $(NETSURF_SRC_DIR)/libsvgtiny/%,$(NETSURF_OBJS)): \
	NETSURF_PRIVATE_CFLAGS := -I $(NETSURF_SRC_DIR)/libsvgtiny/src

# Generate Hubbub's entity trie.
bin/netsurf/libhubbub/src/tokeniser/entities.inc: \
		bin/netsurf/libhubbub/build/Entities \
		bin/netsurf/libhubbub/build/make-entities.pl
	cd bin/netsurf/libhubbub && perl build/make-entities.pl

bin/netsurf/libhubbub/src/treebuilder/autogenerated-element-type.c: \
		bin/netsurf/libhubbub/src/treebuilder/element-type.gperf \
		tools/generate_hubbub_elements.pl
	perl tools/generate_hubbub_elements.pl $< $@

NETSURF_FONT_TOOL := netsurf_compat/convert_font.exe
NETSURF_FONT_STAMP := netsurf_compat/generated/font-ns-sans.stamp

$(NETSURF_FONT_TOOL): bin/netsurf/netsurf/tools/convert_font.c
	gcc -O2 -Wall -Wextra -o $@ $<

$(NETSURF_FONT_STAMP): \
		bin/netsurf/netsurf/frontends/framebuffer/res/fonts/glyph_data \
		$(NETSURF_FONT_TOOL)
	mkdir -p netsurf_compat/generated
	$(NETSURF_FONT_TOOL) -H netsurf_compat/generated/font-ns-sans.h \
		$< netsurf_compat/generated/font-ns-sans.c
	touch $@

netsurf_compat/generated/font-ns-sans.c \
netsurf_compat/generated/font-ns-sans.h: $(NETSURF_FONT_STAMP)

NETSURF_MESSAGES := netsurf_compat/generated/Messages

$(NETSURF_MESSAGES): \
		bin/netsurf/netsurf/resources/FatMessages \
		tools/generate_netsurf_messages.pl
	mkdir -p netsurf_compat/generated
	perl tools/generate_netsurf_messages.pl $< $@

$(NETSURF_GUI): \
		bin/netsurf/netsurf/frontends/framebuffer/gui.c \
		tools/generate_netsurf_gui.ps1
	mkdir -p netsurf_compat/generated
	powershell.exe -NoProfile -ExecutionPolicy Bypass -File \
		tools/generate_netsurf_gui.ps1 \
		-InputPath $< -OutputPath $@

$(NETSURF_FBTK_TEXT): \
		bin/netsurf/netsurf/frontends/framebuffer/fbtk/text.c \
		tools/generate_netsurf_text.ps1
	mkdir -p netsurf_compat/generated
	powershell.exe -NoProfile -ExecutionPolicy Bypass -File \
		tools/generate_netsurf_text.ps1 \
		-InputPath $< -OutputPath $@

define NETSURF_IMAGE_RULE
netsurf_compat/generated/image-$(1).c: \
		bin/netsurf/netsurf/frontends/framebuffer/res/$(2) \
		tools/generate_netsurf_image.ps1
	powershell.exe -NoProfile -ExecutionPolicy Bypass -File \
		tools/generate_netsurf_image.ps1 \
		-InputPath $$< -OutputPath $$@ -Name $(1)
endef

$(eval $(call NETSURF_IMAGE_RULE,left_arrow,icons/back.png))
$(eval $(call NETSURF_IMAGE_RULE,right_arrow,icons/forward.png))
$(eval $(call NETSURF_IMAGE_RULE,reload,icons/reload.png))
$(eval $(call NETSURF_IMAGE_RULE,stop_image,icons/stop.png))
$(eval $(call NETSURF_IMAGE_RULE,history_image,icons/history.png))
$(eval $(call NETSURF_IMAGE_RULE,left_arrow_g,icons/back_g.png))
$(eval $(call NETSURF_IMAGE_RULE,right_arrow_g,icons/forward_g.png))
$(eval $(call NETSURF_IMAGE_RULE,reload_g,icons/reload_g.png))
$(eval $(call NETSURF_IMAGE_RULE,stop_image_g,icons/stop_g.png))
$(eval $(call NETSURF_IMAGE_RULE,history_image_g,icons/history_g.png))
$(eval $(call NETSURF_IMAGE_RULE,scrolll,icons/scrolll.png))
$(eval $(call NETSURF_IMAGE_RULE,scrollr,icons/scrollr.png))
$(eval $(call NETSURF_IMAGE_RULE,scrollu,icons/scrollu.png))
$(eval $(call NETSURF_IMAGE_RULE,scrolld,icons/scrolld.png))
$(eval $(call NETSURF_IMAGE_RULE,osk_image,icons/osk.png))
$(eval $(call NETSURF_IMAGE_RULE,pointer_image,pointers/default.png))
$(eval $(call NETSURF_IMAGE_RULE,hand_image,pointers/point.png))
$(eval $(call NETSURF_IMAGE_RULE,caret_image,pointers/caret.png))
$(eval $(call NETSURF_IMAGE_RULE,menu_image,pointers/menu.png))
$(eval $(call NETSURF_IMAGE_RULE,progress_image,pointers/progress.png))
$(eval $(call NETSURF_IMAGE_RULE,move_image,pointers/move.png))
$(eval $(call NETSURF_IMAGE_RULE,throbber0,throbber/throbber0.png))
$(eval $(call NETSURF_IMAGE_RULE,throbber1,throbber/throbber1.png))
$(eval $(call NETSURF_IMAGE_RULE,throbber2,throbber/throbber2.png))
$(eval $(call NETSURF_IMAGE_RULE,throbber3,throbber/throbber3.png))
$(eval $(call NETSURF_IMAGE_RULE,throbber4,throbber/throbber4.png))
$(eval $(call NETSURF_IMAGE_RULE,throbber5,throbber/throbber5.png))
$(eval $(call NETSURF_IMAGE_RULE,throbber6,throbber/throbber6.png))
$(eval $(call NETSURF_IMAGE_RULE,throbber7,throbber/throbber7.png))
$(eval $(call NETSURF_IMAGE_RULE,throbber8,throbber/throbber8.png))

bin/netsurf/netsurf/frontends/framebuffer/font_internal.tos.o: \
		netsurf_compat/generated/font-ns-sans.h

# Compile after generated sources are ready.
$(NETSURF_OBJS): %.tos.o: %.c \
		bin/netsurf/libhubbub/src/tokeniser/entities.inc \
		bin/netsurf/libhubbub/src/treebuilder/autogenerated-element-type.c
	$(USER_ELF_CC) $(NETSURF_CFLAGS) $(NETSURF_PRIVATE_CFLAGS) $(CCDB) -c $< -o $@

# Link netsurf.elf
NETSURF_COMBINED_OBJ := netsurf_compat/generated/netsurf.tos.o
NETSURF_OBJECTS_RSP := netsurf_compat/generated/netsurf-objects.rsp

$(NETSURF_COMBINED_OBJ): $(NETSURF_OBJS)
	$(file >$(NETSURF_OBJECTS_RSP),$(NETSURF_OBJS))
	$(USER_ELF_LD) -r -o $@ @$(NETSURF_OBJECTS_RSP)

bin/netsurf/netsurf.elf: $(NETSURF_COMBINED_OBJ) $(NETSURF_MESSAGES) $(MUSL_LIBC) $(LIBTOS) $(MUSL_LINK) $(MUSL_LINKER_SCRIPT)
	CC=$(USER_ELF_CC) "$(SHELL)" $(MUSL_LINK) $@ \
		$(NETSURF_COMBINED_OBJ) $(LIBTOS)

# --- PE binaries ---------------------------------------------------------

# Every PE object, whether library or program, comes from a .c of the same
# stem compiled by mingw. The .pe.o suffix keeps it clear of the ELF object
# the same source also produces.
$(PE_LIB_C_OBJS) $(PE_BIN_OBJS) $(PE_VARIANT_OBJS): %.pe.o: %.c
	$(USER_PE_CC) $(PE_CFLAGS) -c $< -o $@


lib/crt0_pe.pe.o: lib/crt0_pe.S
	$(USER_PE_CC) -c $< -o $@

# Link PE executables using MinGW. Never $(LIB_OBJS) , those are ELF
# objects and mingw's ld refuses to put them in a PE image.
#
# -e _start                    : no CRT, so name the entry point ourselves
# -Wl,--image-base,0x140000000 : the default 64-bit PE ImageBase, 5 GiB, well
#                                above every ELF arena , exactly the case
#                                MAP_FIXED and the loader's fixed placement
#                                need to handle
# -Wl,--subsystem,console      : cosmetic here, but keeps the header honest
PE_LDFLAGS := -nostdlib -static -e _start \
              -Wl,--image-base,0x140000000 \
              -Wl,--subsystem,console

# Self-contained programs link no library at all.
$(PE_BIN_EXES): bin/%.exe: bin/%.pe.o
	$(USER_PE_CC) $(PE_LDFLAGS) $^ -o $@

# Variants link the mingw-built library, same as their ELF twins link LIB_OBJS.
$(PE_VARIANT_EXES): bin/%.exe: bin/%.pe.o $(PE_LIB_OBJS)
	$(USER_PE_CC) $(PE_LDFLAGS) $^ -o $@

clean:
	rm -f $(LIBTOS) lib/*.o lib/*.d bin/*/*.o bin/*/*.d bin/*/*/*.o bin/*/*/*.d \
		bin/*/*.elf bin/*/*.exe

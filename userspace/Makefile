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
                  -MMD -MP
USER_LDFLAGS := -nostdlib -static -T lib/user.ld
BUILD_DOOM ?= 0
SHOW_DOOM_INFO ?= 0


# PE binaries cannot link ELF objects, so every object is built twice: once
# by the ELF cross-compiler into foo.o, once by mingw into foo.pe.o. The
# SOURCES are shared — bin/hello/hello.c produces both hello.elf and
# hello.exe — because the syscall layer is typed on sysarg_t rather than
# `long`, which differs in width between the two toolchains. See the note
# in lib/syscall.h.
#
# -fno-asynchronous-unwind-tables drops .pdata/.xdata: nothing unwinds, and
# it keeps the section count down to what the loader has to walk.
PE_CFLAGS := -ffreestanding -nostdlib -fno-pie -mno-red-zone -static \
							-fno-asynchronous-unwind-tables \
              -I . -I lib -I include -I ../kernel -O2 -Wall -Wextra -std=gnu23

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
# things still linking this are thread.elf — which drives TOS's own thread
# syscalls rather than clone(2) — and DOOM, which is a vendored tree nobody
# has re-ported. Do not add binaries here; add them to MUSL_BINS.
#
# string_extra carries strstr/strcpy/strcat/strrchr/strcasecmp, which
# string.c does not define. Without it any tool calling one of those only
# links while the call is optimised away.
# stat.o is in the base set, not the DOOM-only extras: stat/fstat are real
# syscalls now, so every binary should be able to call them.
# time_stub carries time()/clock_gettime()/time_to_calendar(). It used to be
# DOOM-only, which meant any other binary calling time() failed to link.
LIB_OBJS := lib/crt0.o lib/syscall_asm.o lib/syscall.o lib/syscall_posix.o lib/wm.o \
            lib/printf.o lib/malloc.o lib/stdio.o lib/stdio_extra.o lib/string.o lib/keymap.o lib/console.o \
            lib/stdlib_extra.o lib/string_extra.o lib/stat.o lib/bmp.o lib/gfx.o lib/ui.o \
            lib/pthread.o lib/time_stub.o

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
# The rule below builds one .c into one .elf. A program with several
# translation units needs its own rule — see holyd further down.
MUSL_DIR      := libc/build-musl-tos
MUSL_SRC_DIR  := libc/musl-1.2.6
MUSL_LINK     := libc/link_tos_musl.sh
MUSL_LIBC     := $(MUSL_DIR)/lib/libc.a
MUSL_BINS     := cat plist hello ls pkill gfx btop uidemo deskelf notepad tree \
                 shutdown reboot fdchild faulter mmaptest audiotest \
                 stress stress_peer mtest vmtest muse sh winman netmon
MUSL_BIN_ELFS := $(foreach b,$(MUSL_BINS),bin/$(b)/$(b).elf)

# libtos.a — the parts of lib/ that musl does not provide: winman IPC, the
# framebuffer, the console, audio, TOS-only syscalls, and the drawing and
# font helpers. Everything here is TOS-specific and collides with nothing in
# libc.a; the POSIX wrappers that did collide live in syscall_posix.c, which
# a musl binary never links.
#
# Built from the same sources as the legacy objects but against musl's
# headers, so a .tos.o is not interchangeable with a .o — same pattern as
# the .pe.o variants above.
LIBTOS_SRCS := lib/syscall.c lib/wm.c lib/gfx.c lib/ui.c lib/ttf.c \
               lib/bmp.c lib/keymap.c lib/console.c lib/time_stub.c
LIBTOS_COBJS := $(LIBTOS_SRCS:.c=.tos.o)
# syscall_asm.o is assembly and identical either way — reuse it rather than
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
               -O2 -Wall -Wextra -std=gnu11 \
               $(MUSL_INC) -I . -I lib -I include -I ../kernel \
               -DTOS_USE_MUSL

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

HOLYD_SRC_DIR := bin/holyd
# Find all .c files in the holyd root and its subdirectories
HOLYD_SOURCES := $(wildcard $(HOLYD_SRC_DIR)/*.c) \
                 $(wildcard $(HOLYD_SRC_DIR)/lexer/*.c) \
                 $(wildcard $(HOLYD_SRC_DIR)/parser/*.c) \
                 $(wildcard $(HOLYD_SRC_DIR)/ast/*.c)
# holyd is the one musl program built from more than one translation unit,
# so it compiles to .tos.o objects of its own rather than going through the
# one-source-per-binary MUSL_BIN rule below.
HOLYD_OBJS := $(HOLYD_SOURCES:.c=.tos.o)
HOLYD_CFLAGS := $(MUSL_CFLAGS) -I $(HOLYD_SRC_DIR) -MMD -MP

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
# rule). Uses DOOM_CFLAGS by design — anything compiled with USER_CFLAGS must
# be in LIB_C_OBJS or SIMPLE_BIN_OBJS so the static patterns below claim it.
%.o: %.c
	$(USER_ELF_CC) $(DOOM_CFLAGS) -c $< -o $@

USERSPACE_TARGETS := $(SIMPLE_BIN_ELFS) $(MUSL_BIN_ELFS) $(PE_BIN_EXES) $(PE_VARIANT_EXES) bin/holyd/holyd.elf
ifeq ($(BUILD_DOOM),1)
USERSPACE_TARGETS += bin/doom/doom.elf
endif

.PHONY: all clean doom
all: $(USERSPACE_TARGETS)

doom: bin/doom/doom.elf

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

# Header dependencies generated by -MMD. Without these a change to a header
# — winman.h being the one that bites, since winman.c is almost all of it —
# leaves a stale .o in place and the disk image ships last build's binary.
# Leading '-' so a clean tree with no .d files yet is not an error.
USER_DEP_FILES := $(patsubst %.o,%.d,$(LIB_C_OBJS) $(SIMPLE_BIN_OBJS) $(HOLYD_OBJS))
-include $(USER_DEP_FILES)

# Static pattern for the link step. bin/foo/foo.elf <- bin/foo/foo.o + lib.
$(filter-out $(TTF_BIN_ELFS),$(SIMPLE_BIN_ELFS)): bin/%.elf: bin/%.o $(LIB_OBJS)
	$(USER_ELF_LD) $(USER_LDFLAGS) $^ -o $@

# Same link, plus the font rasteriser. ld pulls in whole objects rather than
# archive members, so ttf.o stays off the default line — it is 70 KiB that
# every tool not drawing glyphs would otherwise carry.
$(TTF_BIN_ELFS): bin/%.elf: bin/%.o $(LIB_OBJS) $(TTF_OBJS)
	$(USER_ELF_LD) $(USER_LDFLAGS) $^ -o $@

# --- musl-linked binaries ------------------------------------------------

# link_tos_musl.sh owns the include paths and the crt1/crti/libc.a/crtn
# ordering, and builds musl on demand if the archive is missing. Going
# through it keeps one copy of those flags rather than a second set here
# that could drift.
$(LIBTOS_COBJS): %.tos.o: %.c
	$(USER_ELF_CC) $(MUSL_CFLAGS) -c $< -o $@

$(LIBTOS): $(LIBTOS_OBJS)
	rm -f $@ && x86_64-elf-ar rcs $@ $^

$(MUSL_BIN_ELFS): bin/%.elf: bin/%.c $(MUSL_LIBC) $(LIBTOS)
	CC=$(USER_ELF_CC) "$(SHELL)" $(MUSL_LINK) $@ $< $(LIBTOS)

# --- holyd rules ---------------------------------------------------------

# HolyD headers use paths rooted at bin/holyd (for example, lexer/lexer.h).
$(HOLYD_OBJS): %.tos.o: %.c
	$(USER_ELF_CC) $(HOLYD_CFLAGS) -c $< -o $@

# Link holyd.elf. Objects, not a source, so link_tos_musl.sh gets them
# straight through — it passes anything after the output name to the
# compiler driver unchanged.
bin/holyd/holyd.elf: $(HOLYD_OBJS) $(MUSL_LIBC) $(LIBTOS)
	CC=$(USER_ELF_CC) "$(SHELL)" $(MUSL_LINK) $@ $(HOLYD_OBJS) $(LIBTOS)

# --- PE binaries ---------------------------------------------------------

# Every PE object, whether library or program, comes from a .c of the same
# stem compiled by mingw. The .pe.o suffix keeps it clear of the ELF object
# the same source also produces.
$(PE_LIB_C_OBJS) $(PE_BIN_OBJS) $(PE_VARIANT_OBJS): %.pe.o: %.c
	$(USER_PE_CC) $(PE_CFLAGS) -c $< -o $@


lib/crt0_pe.pe.o: lib/crt0_pe.S
	$(USER_PE_CC) -c $< -o $@

# Link PE executables using MinGW. Never $(LIB_OBJS) — those are ELF
# objects and mingw's ld refuses to put them in a PE image.
#
# -e _start                    : no CRT, so name the entry point ourselves
# -Wl,--image-base,0x140000000 : the default 64-bit PE ImageBase, 5 GiB, well
#                                above every ELF arena — exactly the case
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

SHELL := /usr/bin/bash
USER_ELF_CC  := x86_64-elf-gcc
USER_ELF_LD  := x86_64-elf-ld
USER_PE_CC   := x86_64-w64-mingw32-gcc
USER_AS      := nasm
USER_CFLAGS  := -ffreestanding -nostdlib -fno-pie -mno-red-zone -static \
                  -I . -I lib -I include -I ../kernel -O2 -Wall -Wextra -std=gnu23
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

# string_extra carries strstr/strcpy/strcat/strrchr/strcasecmp, which
# string.c does not define. Without it any tool calling one of those only
# links while the call is optimised away — which is how plist and pkill
# built despite referencing strstr.
# stat.o is in the base set, not the DOOM-only extras: stat/fstat are real
# syscalls now, so every binary should be able to call them.
LIB_OBJS := lib/crt0.o lib/syscall_asm.o lib/syscall.o lib/wm.o \
            lib/printf.o lib/malloc.o lib/stdio.o lib/string.o lib/keymap.o lib/console.o \
            lib/stdlib_extra.o lib/string_extra.o lib/stat.o lib/bmp.o lib/gfx.o lib/ui.o lib/pthread.o

TTF_OBJS := lib/ttf.o

# C-source lib objects (.s sources have their own explicit rules below).
LIB_C_OBJS := $(filter-out lib/crt0.o lib/syscall_asm.o, $(LIB_OBJS)) $(TTF_OBJS)

# One binary per directory under bin/, named after the directory.
# Doom is special (multi-source) and gets its own rules.
SIMPLE_BINS := hello cat gfx ls sh shutdown reboot mtest btop pkill \
							plist fdchild vmtest uidemo thread deskelf stress \
						 	stress_peer audiotest muse mmaptest faulter

SIMPLE_BIN_OBJS := $(foreach b,$(SIMPLE_BINS),bin/$(b)/$(b).o)
SIMPLE_BIN_ELFS := $(foreach b,$(SIMPLE_BINS),bin/$(b)/$(b).elf)
WINMAN_OBJ := bin/winman/winman.o
WINMAN_ELF := bin/winman/winman.elf

HOLYD_SRC_DIR := bin/holyd
# Find all .c files in the holyd root and its subdirectories
HOLYD_SOURCES := $(wildcard $(HOLYD_SRC_DIR)/*.c) \
                 $(wildcard $(HOLYD_SRC_DIR)/lexer/*.c) \
                 $(wildcard $(HOLYD_SRC_DIR)/parser/*.c) \
                 $(wildcard $(HOLYD_SRC_DIR)/ast/*.c)
HOLYD_OBJS := $(HOLYD_SOURCES:.c=.o)
HOLYD_CFLAGS := $(USER_CFLAGS) -I $(HOLYD_SRC_DIR)

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
                   lib/time_stub.o lib/unistd_stub.o

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

USERSPACE_TARGETS := $(SIMPLE_BIN_ELFS) $(WINMAN_ELF) $(PE_BIN_EXES) $(PE_VARIANT_EXES) bin/holyd/holyd.elf
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
$(SIMPLE_BIN_OBJS) $(WINMAN_OBJ): %.o: %.c
	$(USER_ELF_CC) $(USER_CFLAGS) -c $< -o $@

# Static pattern for the link step. bin/foo/foo.elf <- bin/foo/foo.o + lib.
$(SIMPLE_BIN_ELFS): bin/%.elf: bin/%.o $(LIB_OBJS)
	$(USER_ELF_LD) $(USER_LDFLAGS) $^ -o $@

$(WINMAN_ELF): $(WINMAN_OBJ) $(LIB_OBJS) $(TTF_OBJS)
	$(USER_ELF_LD) $(USER_LDFLAGS) $^ -o $@

# --- holyd rules ---------------------------------------------------------

# HolyD headers use paths rooted at bin/holyd (for example, lexer/lexer.h).
$(HOLYD_OBJS): %.o: %.c
	$(USER_ELF_CC) $(HOLYD_CFLAGS) -c $< -o $@

# Link holyd.elf
bin/holyd/holyd.elf: $(HOLYD_OBJS) $(LIB_OBJS)
	$(USER_ELF_LD) $(USER_LDFLAGS) $^ -o $@

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
	rm -f lib/*.o bin/*/*.o bin/*/*/*.o bin/*/*.elf bin/*/*.exe

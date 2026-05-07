SHELL := /usr/bin/bash
USER_CC      := x86_64-elf-gcc
USER_LD      := x86_64-elf-ld
USER_AS      := nasm
USER_CFLAGS  := -ffreestanding -nostdlib -fno-pie -mno-red-zone -static \
                -I lib -O2 -Wall -Wextra
USER_LDFLAGS := -nostdlib -static -T lib/user.ld

LIB_OBJS := lib/crt0.o lib/syscall_asm.o lib/syscall.o \
            lib/printf.o lib/malloc.o lib/stdio.o lib/string.o lib/keymap.o lib/console.o \
            lib/stdlib_extra.o

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
                   lib/time_stub.o lib/unistd_stub.o lib/stat_stub.o

DOOM_CFLAGS := -ffreestanding -nostdlib -fno-pie -mno-red-zone -static \
                -I include -I lib \
                -I $(DOOM_SRC_DIR) \
                -O2 -Wno-error \
                -DNORMALUNIX -DLINUX


bin/doom/doom.elf: $(DOOM_OBJS) $(DOOM_PLATFORM_OBJS) $(LIB_OBJS) $(DOOM_EXTRA_OBJS)
	$(USER_LD) $(USER_LDFLAGS) $^ -o $@

$(info DOOM_SRC_DIR = $(DOOM_SRC_DIR))
$(info DOOM_SOURCES count = $(words $(DOOM_SOURCES)))
$(info DOOM_OBJS first = $(firstword $(DOOM_OBJS)))

%.o: %.c
	$(USER_CC) $(DOOM_CFLAGS) -c $< -o $@

.PHONY: all clean
all: bin/hello/hello.elf bin/cat/cat.elf bin/gfx/gfx.elf bin/doom/doom.elf bin/ls/ls.elf bin/sh/sh.elf bin/shutdown/shutdown.elf bin/reboot/reboot.elf

# library objects
lib/crt0.o: lib/crt0.s
	$(USER_AS) -f elf64 $< -o $@

lib/syscall_asm.o: lib/syscall.s
	$(USER_AS) -f elf64 $< -o $@

lib/syscall.o: lib/syscall.c
	$(USER_CC) $(USER_CFLAGS) -c $< -o $@

lib/printf.o: lib/printf.c
	$(USER_CC) $(USER_CFLAGS) -c $< -o $@

lib/malloc.o: lib/malloc.c
	$(USER_CC) $(USER_CFLAGS) -c $< -o $@

lib/stdio.o: lib/stdio.c
	$(USER_CC) $(USER_CFLAGS) -c $< -o $@

lib/string.o: lib/string.c
	$(USER_CC) $(USER_CFLAGS) -c $< -o $@

lib/keymap.o: lib/keymap.c
	$(USER_CC) $(USER_CFLAGS) -c $< -o $@

lib/console.o: lib/console.c
	$(USER_CC) $(USER_CFLAGS) -c $< -o $@

# program: hello
bin/hello/hello.o: bin/hello/hello.c
	$(USER_CC) $(USER_CFLAGS) -c $< -o $@

bin/hello/hello.elf: bin/hello/hello.o $(LIB_OBJS)
	$(USER_LD) $(USER_LDFLAGS) $^ -o $@

# program: cat
bin/cat/cat.o: bin/cat/cat.c
	$(USER_CC) $(USER_CFLAGS) -c $< -o $@

bin/cat/cat.elf: bin/cat/cat.o $(LIB_OBJS)
	$(USER_LD) $(USER_LDFLAGS) $^ -o $@

# program: gfx test
bin/gfx/gfx.o: bin/gfx/gfx.c
	$(USER_CC) $(USER_CFLAGS) -c $< -o $@

bin/gfx/gfx.elf: bin/gfx/gfx.o $(LIB_OBJS)
	$(USER_LD) $(USER_LDFLAGS) $^ -o $@

# program: ls test
bin/ls/ls.o: bin/ls/ls.c
	$(USER_CC) $(USER_CFLAGS) -c $< -o $@

bin/ls/ls.elf: bin/ls/ls.o $(LIB_OBJS)
	$(USER_LD) $(USER_LDFLAGS) $^ -o $@

# program: tsh shell
bin/sh/sh.o: bin/sh/sh.c
	$(USER_CC) $(USER_CFLAGS) -c $< -o $@

bin/sh/sh.elf: bin/sh/sh.o $(LIB_OBJS)
	$(USER_LD) $(USER_LDFLAGS) $^ -o $@

bin/shutdown/shutdown.o: bin/shutdown/shutdown.c
	$(USER_CC) $(USER_CFLAGS) -c $< -o $@

bin/shutdown/shutdown.elf: bin/shutdown/shutdown.o $(LIB_OBJS)
	$(USER_LD) $(USER_LDFLAGS) $^ -o $@

bin/reboot/reboot.o: bin/reboot/reboot.c
	$(USER_CC) $(USER_CFLAGS) -c $< -o $@

bin/reboot/reboot.elf: bin/reboot/reboot.o $(LIB_OBJS)
	$(USER_LD) $(USER_LDFLAGS) $^ -o $@

clean:
	rm -f lib/*.o bin/*/*.o bin/*/*.elf

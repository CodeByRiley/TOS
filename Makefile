# I FUCKING HATE MAKE IT IS THE WORST BUILD SYSTEM EVER
rwildcard = $(foreach d,$(wildcard $1*),$(call rwildcard,$d/,$2) $(filter $(subst *,%,$2),$d))

# Kernel C sources and their headers live together under kernel/<subsystem>/.
# Architecture assembly lives under kernel/arch/x86_64/.
# offsets.c exists to be compiled with -S and scraped for NASM %defines; it
# has no code and must never be linked into the kernel.
asm_offsets_src := kernel/arch/offsets.c
kernel_c_source_files := $(filter-out $(asm_offsets_src) kernel/fs_old/%, $(call rwildcard,kernel/,*.c))
kernel_c_object_files := $(patsubst kernel/%.c, build/kernel/%.o, $(kernel_c_source_files))

# AP trampoline is assembled flat (org 0x8000, real-mode start) , never elf64.
# Filter it out of the regular asm rule and build it via the explicit
# ap_trampoline.bin -> ap_trampoline.o pipeline below.
ap_trampoline_src := kernel/arch/x86_64/boot/ap_trampoline.asm
ap_trampoline_bin := build/kernel/arch/x86_64/boot/ap_trampoline.bin
ap_trampoline_obj := build/kernel/arch/x86_64/boot/ap_trampoline.o
kernel_asm_source_files := $(filter-out $(ap_trampoline_src), $(call rwildcard,kernel/,*.asm))
kernel_asm_object_files := $(patsubst kernel/%.asm, build/kernel/%.o, $(kernel_asm_source_files))

kernel_object_files := $(kernel_c_object_files) $(kernel_asm_object_files) $(ap_trampoline_obj)

# Emit header dependencies for incremental rebuilds.
kernel_c_flags := -I kernel -ffreestanding -mno-red-zone -mcmodel=kernel -fno-pic -fno-pie -MMD -MP -std=gnu23 -Og

kernel_dep_files := $(kernel_c_object_files:.o=.d)

# Constants shared between C structs and hand-written assembly. See
# kernel/arch/offsets.c for why these are generated rather than checked in.
asm_offsets_asm := build/generated/asm_offsets.s
asm_offsets_inc := build/generated/asm_offsets.inc
asm_offsets_dep := build/generated/asm_offsets.d

CCDB = -MJ $@.json

linker_script   := boot/x86_64/linker.ld
iso_dir         := boot/x86_64/iso
kernel_bin      := dist/x86_64/kernel.bin
ROOTFS_TYPE     ?= fat
disk_img        := build/disk-$(ROOTFS_TYPE).img
USERSPACE_CLEAN ?= 0
BUILD_DOOM      ?= 0
BUILD_NETSURF   ?= 0

# The disk image, the ISO and clean need POSIX tools that Windows does not
# ship and MSYS2 does not package: xorriso, mtools, and a GRUB install with
# i386-pc modules. Sitting in an MSYS2 or Git Bash shell is therefore not a
# reason to run them natively , that shell has bash but not the tools, and it
# picks up whatever unrelated grub happens to be on PATH, which fails much
# later with things like "kernel.img is miscompiled". On Windows these steps
# always go through WSL.
#
# TOS_NATIVE_TOOLS=1 opts out, for a shell that genuinely has all of them.
TOS_NATIVE_TOOLS ?= 0

# mingw32-make sees OS=Windows_NT; MSYS2's own make does not, but it does set
# MSYSTEM. Either one means we are on Windows and the tools live in WSL.
windows_host := $(if $(filter Windows_NT,$(OS))$(MSYSTEM),1,)
NASM ?= nasm

ifeq ($(windows_host),1)
  ifeq ($(TOS_NATIVE_TOOLS),1)
    run_linux = $(1)
  else
    # wslpath only understands drive-letter paths, so mingw32-make's
    # C:/... CURDIR works and MSYS2 make's /c/... one silently converts to
    # the wrong directory. Catch that here rather than in a build 40 lines on.
    ifeq ($(findstring :,$(CURDIR)),)
      $(error CURDIR is "$(CURDIR)", which wslpath cannot convert. Build with \
        mingw32-make (MinGW), not MSYS2's own make)
    endif
    run_linux = wsl bash -lc "cd \"\$$(wslpath -a '$(CURDIR)')\" && $(1)"
    # Keep the cross compiler on Windows, but use WSL's assembler when NASM
    # is not part of the native toolchain.
    NASM := wsl nasm
  endif
else
  # Linux: native.
  run_linux = $(1)
endif

nvidia_firmware_files := $(wildcard rootfs/firmware/*.bin)
# The .hd scripts live in the HolyD submodule now, not under rootfs.
holyd_sample_files := $(wildcard userspace/bin/holyd/tests/*.hd) \
                      $(wildcard userspace/bin/holyd/samples/*.hd)
icon_files := $(wildcard rootfs/system/icons/*.bmp)
rootfs_payload_files := rootfs/readme.txt \
                        $(icon_files) \
                        $(holyd_sample_files) \
                        $(nvidia_firmware_files)

$(kernel_c_object_files): build/kernel/%.o : kernel/%.c
	mkdir -p $(dir $@) && \
	x86_64-elf-gcc -c $(kernel_c_flags) $(patsubst build/kernel/%.o, kernel/%.c, $@) -o $@

# Pull in the generated header dependencies. Leading '-' so a clean tree (no
# .d files yet) is not an error.
-include $(kernel_dep_files)

# -S only: nothing here is assembled, the markers are scraped out of the text.
$(asm_offsets_asm): $(asm_offsets_src)
	mkdir -p $(dir $@)
	x86_64-elf-gcc -S $(kernel_c_flags) $< -o $@

$(asm_offsets_inc): $(asm_offsets_asm) tools/gen_asm_offsets.py
	mkdir -p $(dir $@)
	python tools/gen_asm_offsets.py $< $@

-include $(asm_offsets_dep)

# Any .asm file may include the generated offsets, so all of them wait for
# it and all of them get its directory on the NASM include path.
$(kernel_asm_object_files) $(ap_trampoline_bin): $(asm_offsets_inc)

$(kernel_asm_object_files): build/kernel/%.o : kernel/%.asm
	mkdir -p $(dir $@) && \
	$(NASM) -f elf64 -i $(dir $(asm_offsets_inc)) \
		$(patsubst build/kernel/%.o, kernel/%.asm, $@) -o $@

# AP trampoline: flat 16/32/64-bit blob, wrapped as an ELF .rodata symbol so
# the kernel can `memcpy` it to physical 0x8000 before INIT-SIPI-SIPI.
$(ap_trampoline_bin): $(ap_trampoline_src)
	mkdir -p $(dir $@) && $(NASM) -f bin -i $(dir $(asm_offsets_inc)) $< -o $@

# objcopy derives the _binary_* symbol names from the input path, so run it
# from the blob's own directory: the symbols stay _binary_ap_trampoline_bin_*
# no matter where build output moves. kernel/sched/smp.c declares those names.
$(ap_trampoline_obj): $(ap_trampoline_bin)
	cd $(dir $<) && x86_64-elf-objcopy -I binary -O elf64-x86-64 -B i386 \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents \
		$(notdir $<) $(notdir $@)

.PHONY: userspace
userspace:
ifeq ($(USERSPACE_CLEAN),1)
	$(MAKE) -C userspace clean
endif
	$(MAKE) -C userspace all BUILD_DOOM=$(BUILD_DOOM) BUILD_NETSURF=$(BUILD_NETSURF)


$(disk_img): userspace tools/create_disk.sh $(rootfs_payload_files) | $(kernel_bin)
	@echo "Creating Disk Image"
	@mkdir -p $(dir $@)
	@$(call run_linux,IMG=$(disk_img) TOS_ROOTFS_TYPE=$(ROOTFS_TYPE) bash tools/create_disk.sh)
	@test -s "$@"
	@echo "Disk Image Finished"

# --- Kernel Symbol Table (Two-pass link) -----------------------------
# Link once to generate symbols, then relink with the generated table.
kernel_nosyms_elf := build/kernel_nosyms.elf
symtab_gen_src := build/generated/symtab.c
symtab_gen_obj := build/generated/symtab.o

$(kernel_nosyms_elf): $(kernel_object_files) $(linker_script)
	mkdir -p $(dir $@)
	x86_64-elf-ld -z max-page-size=0x1000 -o $@ -T $(linker_script) \
	    $(kernel_object_files)

$(symtab_gen_src): $(kernel_nosyms_elf) tools/gen_symtab.py
	mkdir -p $(dir $@)
	tools/gen_symtab.py $(kernel_nosyms_elf) $@

$(symtab_gen_obj): $(symtab_gen_src)
	mkdir -p $(dir $@)
	x86_64-elf-gcc -c $(kernel_c_flags) $< -o $@

# Link only , no disk image, no ISO. Useful for a quick compile check.
.PHONY: kernel
kernel: $(kernel_bin)

.PHONY: clangd
clangd:
	python tools/gen_compile_commands.py

$(kernel_bin): $(kernel_object_files) $(symtab_gen_obj) $(linker_script)
	@echo "==> Linking $@"
	mkdir -p $(dir $@)
	x86_64-elf-ld -z max-page-size=0x1000 -o $@ -T $(linker_script) \
	    $(kernel_object_files) $(symtab_gen_obj)
	@echo "==> Finished $@"

.PHONY: build-x86_64
build-x86_64: $(kernel_bin) $(disk_img)
	$(call run_linux,DISK_IMAGE=$(disk_img) bash tools/build_iso.sh)

# --- host + QEMU regression suites -------------------------------------
HOST_CC ?= gcc
HOST_TEST_CFLAGS := -std=c11 -O2 -Wall -Wextra
HOST_TEST_DIR := build/tests

# Host stand-ins for the kernel allocator and log. Linked by the tests that
# compile kernel sources; a test wanting to assert on either defines its own
# and leaves this out.
HOST_KERNEL_STUBS := tests/host_kernel_stubs.c

VFS_HOST_SRCS := kernel/fs/vfs/vfs.c kernel/fs/vfs/namei.c kernel/fs/vfs/file.c \
		kernel/fs/vfs/lock.c tests/vfs_lock_host.c
VFS_HOST_TESTS := $(addprefix $(HOST_TEST_DIR)/,vfs_test.exe fat_directory_test.exe \
		ext2_vfs_test.exe ext2_device_test.exe stdio_mode_test.exe vfs_serialization_test.exe)
$(VFS_HOST_TESTS): HOST_TEST_CFLAGS += -DVFS_HOST_TEST -pthread
$(VFS_HOST_TESTS): kernel/fs/vfs/lock.h kernel/sched/sched.h tests/vfs_lock_host.h
FAT_HOST_SRCS := kernel/fs/fat/fat.c kernel/fs/fat/fat_mount.c \
		kernel/fs/fat/fat_file.c kernel/fs/fat/fat_name.c \
		kernel/fs/fat/fat_directory.c kernel/fs/fat/fat_vfs.c
FS_HOST_HEADERS := $(wildcard kernel/fs/vfs/*.h kernel/fs/fat/*.h kernel/fs/ext2/*.h)

HOST_TEST_BINS := \
	$(HOST_TEST_DIR)/vfs_serialization_test.exe \
	$(HOST_TEST_DIR)/vfs_test.exe \
	$(HOST_TEST_DIR)/pmm_test.exe \
	$(HOST_TEST_DIR)/vmm_test.exe \
	$(HOST_TEST_DIR)/process_pml4_test.exe \
	$(HOST_TEST_DIR)/fat_directory_test.exe \
	$(HOST_TEST_DIR)/ext2_vfs_test.exe \
	$(HOST_TEST_DIR)/ext2_device_test.exe \
	$(HOST_TEST_DIR)/usb_storage_test.exe \
	$(HOST_TEST_DIR)/stdio_mode_test.exe \
	$(HOST_TEST_DIR)/bmp_decode_test.exe \
	$(HOST_TEST_DIR)/gfx_ui_test.exe \
	$(HOST_TEST_DIR)/userspace_runtime_test.exe \
	$(HOST_TEST_DIR)/fb_damage_test.exe \
	$(HOST_TEST_DIR)/holyd_compiler_test.exe

$(HOST_TEST_DIR):
	mkdir -p $@

$(HOST_TEST_DIR)/vfs_test.exe: tests/vfs_test.c $(VFS_HOST_SRCS) \
		kernel/fs/vfs/vfs.h kernel/fs/vfs/internal.h | $(HOST_TEST_DIR)
	$(HOST_CC) $(HOST_TEST_CFLAGS) -I kernel tests/vfs_test.c $(VFS_HOST_SRCS) -o $@

$(HOST_TEST_DIR)/pmm_test.exe: tests/pmm_test.c kernel/memory/pmm.c \
		kernel/memory/pmm.h | $(HOST_TEST_DIR)
	$(HOST_CC) $(HOST_TEST_CFLAGS) -DPMM_HOST_TEST -I kernel \
		tests/pmm_test.c kernel/memory/pmm.c -o $@

$(HOST_TEST_DIR)/vmm_test.exe: tests/vmm_test.c kernel/memory/vmm.c \
		kernel/memory/vmm.h kernel/memory/hhdm.h | $(HOST_TEST_DIR)
	$(HOST_CC) $(HOST_TEST_CFLAGS) -DHHDM_HOST_TEST -DVMM_HOST_TEST \
		-I kernel tests/vmm_test.c kernel/memory/vmm.c -o $@

$(HOST_TEST_DIR)/process_pml4_test.exe: tests/process_pml4_test.c \
		kernel/loader/process.c kernel/memory/vmm.h kernel/memory/hhdm.h \
		| $(HOST_TEST_DIR)
	$(HOST_CC) $(HOST_TEST_CFLAGS) -DHHDM_HOST_TEST \
		-DPROCESS_PML4_HOST_TEST -I kernel \
		tests/process_pml4_test.c kernel/loader/process.c -o $@

$(HOST_TEST_DIR)/fat_directory_test.exe: tests/fat_directory_test.c \
		$(HOST_KERNEL_STUBS) $(FAT_HOST_SRCS) $(VFS_HOST_SRCS) \
		$(FS_HOST_HEADERS) tests/vfs_backend_checks.h \
		| $(HOST_TEST_DIR)
	$(HOST_CC) $(HOST_TEST_CFLAGS) -I kernel \
		tests/fat_directory_test.c $(FAT_HOST_SRCS) $(VFS_HOST_SRCS) \
		$(HOST_KERNEL_STUBS) -o $@

$(HOST_TEST_DIR)/ext2-base.img: tools/create_ext2_test_image.sh \
		tests/fixtures/ext2_root/seed/hello.txt | $(HOST_TEST_DIR)
	$(call run_linux,bash tools/create_ext2_test_image.sh $@)

EXT2_HOST_SRCS := kernel/fs/ext2/ext2_mount.c kernel/fs/ext2/ext2_inode.c \
		kernel/fs/ext2/ext2_io.c \
		kernel/fs/ext2/ext2_dir.c kernel/fs/ext2/ext2_file.c \
		kernel/fs/ext2/ext2_vfs.c $(VFS_HOST_SRCS)

$(HOST_TEST_DIR)/vfs_serialization_test.exe: tests/vfs_serialization_test.c \
		$(EXT2_HOST_SRCS) $(HOST_KERNEL_STUBS) $(FS_HOST_HEADERS) \
		$(HOST_TEST_DIR)/ext2-base.img | $(HOST_TEST_DIR)
	$(HOST_CC) $(HOST_TEST_CFLAGS) -I kernel tests/vfs_serialization_test.c \
		$(EXT2_HOST_SRCS) $(HOST_KERNEL_STUBS) -o $@

$(HOST_TEST_DIR)/ext2_vfs_test.exe: tests/ext2_vfs_test.c $(EXT2_HOST_SRCS) \
		tests/vfs_backend_checks.h $(FS_HOST_HEADERS) \
		$(HOST_KERNEL_STUBS) $(HOST_TEST_DIR)/ext2-base.img | $(HOST_TEST_DIR)
	$(HOST_CC) $(HOST_TEST_CFLAGS) -I kernel tests/ext2_vfs_test.c \
		$(EXT2_HOST_SRCS) $(HOST_KERNEL_STUBS) -o $@

$(HOST_TEST_DIR)/stdio_mode_test.exe: tests/stdio_mode_test.c \
		$(HOST_KERNEL_STUBS) kernel/fs/stdio.c $(FAT_HOST_SRCS) $(VFS_HOST_SRCS) \
		kernel/fs/stdio.h $(FS_HOST_HEADERS) \
		| $(HOST_TEST_DIR)
	$(HOST_CC) $(HOST_TEST_CFLAGS) -fno-builtin -I kernel tests/stdio_mode_test.c \
		kernel/fs/stdio.c $(FAT_HOST_SRCS) $(VFS_HOST_SRCS) $(HOST_KERNEL_STUBS) -o $@

$(HOST_TEST_DIR)/ext2_device_test.exe: tests/ext2_device_test.c $(EXT2_HOST_SRCS) \
		$(FS_HOST_HEADERS) tests/vfs_backend_checks.h kernel/drivers/storage/block.h \
		$(HOST_KERNEL_STUBS) $(HOST_TEST_DIR)/ext2-base.img | $(HOST_TEST_DIR)
	$(HOST_CC) $(HOST_TEST_CFLAGS) -I kernel tests/ext2_device_test.c \
		$(EXT2_HOST_SRCS) $(HOST_KERNEL_STUBS) -o $@

USB_STORAGE_HOST_SRCS := kernel/drivers/usb/storage/bot.c \
		kernel/drivers/usb/storage/scsi.c kernel/drivers/usb/storage/usb_storage.c
$(HOST_TEST_DIR)/usb_storage_test.exe: tests/usb_storage_test.c $(USB_STORAGE_HOST_SRCS) \
		kernel/drivers/usb/storage/usb_storage.h kernel/drivers/storage/block.h \
		kernel/devices/usb.h kernel/sync/spinlock.h | $(HOST_TEST_DIR)
	$(HOST_CC) $(HOST_TEST_CFLAGS) -I kernel tests/usb_storage_test.c \
		$(USB_STORAGE_HOST_SRCS) -o $@

.PHONY: test-storage test-fs
$(HOST_TEST_DIR)/ext2-2k-base.img: tools/create_ext2_test_image.sh \
		tests/fixtures/ext2_root/seed/hello.txt | $(HOST_TEST_DIR)
	$(call run_linux,bash tools/create_ext2_test_image.sh $@ 2048 ^filetype)

$(HOST_TEST_DIR)/ext2-4k-base.img: tools/create_ext2_test_image.sh \
		tests/fixtures/ext2_root/seed/hello.txt | $(HOST_TEST_DIR)
	$(call run_linux,bash tools/create_ext2_test_image.sh $@ 4096)

test-storage: test-fs $(HOST_TEST_DIR)/usb_storage_test.exe \
		$(HOST_TEST_DIR)/ext2-2k-base.img $(HOST_TEST_DIR)/ext2-4k-base.img
	$(HOST_TEST_DIR)/usb_storage_test.exe
	$(HOST_TEST_DIR)/ext2_device_test.exe build/tests/ext2-2k-base.img build/tests/ext2-2k-device.img
	$(HOST_TEST_DIR)/ext2_device_test.exe build/tests/ext2-4k-base.img build/tests/ext2-4k-device.img
	$(call run_linux,e2fsck -fn build/tests/ext2-2k-device.img)
	$(call run_linux,e2fsck -fn build/tests/ext2-4k-device.img)

.PHONY: test-fs
test-fs: $(HOST_TEST_DIR)/vfs_test.exe $(HOST_TEST_DIR)/fat_directory_test.exe \
		$(HOST_TEST_DIR)/vfs_serialization_test.exe \
		$(HOST_TEST_DIR)/ext2_device_test.exe \
		$(HOST_TEST_DIR)/ext2_vfs_test.exe $(HOST_TEST_DIR)/stdio_mode_test.exe
	$(HOST_TEST_DIR)/vfs_test.exe
	$(HOST_TEST_DIR)/vfs_serialization_test.exe
	@for mode in recursive helper unlock irq ap; do \
		$(HOST_TEST_DIR)/vfs_serialization_test.exe $$mode; \
		code=$$?; test $$code -eq 86 || exit 1; \
	done
	$(HOST_TEST_DIR)/fat_directory_test.exe
	$(HOST_TEST_DIR)/ext2_vfs_test.exe
	$(HOST_TEST_DIR)/ext2_device_test.exe
	$(HOST_TEST_DIR)/stdio_mode_test.exe
	$(call run_linux,e2fsck -fn build/tests/ext2-mutated.img)
	$(call run_linux,e2fsck -fn build/tests/ext2-device.img)

$(HOST_TEST_DIR)/bmp_decode_test.exe: tests/bmp_decode_test.c \
		userspace/lib/bmp.c userspace/lib/bmp.h | $(HOST_TEST_DIR)
	$(HOST_CC) $(HOST_TEST_CFLAGS) -I userspace -I userspace/lib \
		tests/bmp_decode_test.c userspace/lib/bmp.c -o $@

$(HOST_TEST_DIR)/gfx_ui_test.exe: tests/gfx_ui_test.c userspace/lib/gfx.c \
		userspace/lib/ui.c userspace/lib/bmp.c userspace/lib/damage.c \
		userspace/lib/page_alloc.c | $(HOST_TEST_DIR)
	$(HOST_CC) $(HOST_TEST_CFLAGS) -I userspace -I userspace/lib tests/gfx_ui_test.c \
		userspace/lib/gfx.c userspace/lib/ui.c userspace/lib/bmp.c \
		userspace/lib/damage.c userspace/lib/page_alloc.c -o $@

$(HOST_TEST_DIR)/userspace_runtime_test.exe: tests/userspace_runtime_test.c \
		userspace/lib/event.c userspace/lib/wm.c userspace/lib/app.c \
		userspace/lib/process.c | $(HOST_TEST_DIR)
	# The host linker has no user.ld; represent an empty app-info section.
	$(HOST_CC) $(HOST_TEST_CFLAGS) -I userspace -I userspace/lib \
		tests/userspace_runtime_test.c userspace/lib/event.c userspace/lib/wm.c \
		userspace/lib/app.c userspace/lib/process.c \
		-Wl,--defsym=__appinfo_start=0,--defsym=__appinfo_end=0 -o $@

$(HOST_TEST_DIR)/fb_damage_test.exe: tests/fb_damage_test.c | $(HOST_TEST_DIR)
	$(HOST_CC) $(HOST_TEST_CFLAGS) $< -o $@

$(HOST_TEST_DIR)/holyd_compiler_test.exe: tests/holyd_compiler_test.c \
		userspace/bin/holyd/src/compiler.c userspace/bin/holyd/src/compiler.h \
		userspace/bin/holyd/src/eval.c userspace/bin/holyd/src/eval.h \
		userspace/bin/holyd/src/runtime.c userspace/bin/holyd/src/runtime.h \
		userspace/bin/holyd/src/lexer/lexer.c userspace/bin/holyd/src/lexer/lexer.h \
		userspace/bin/holyd/src/parser/parser.c userspace/bin/holyd/src/parser/parser.h \
		userspace/bin/holyd/src/ast/ast.c userspace/bin/holyd/src/ast/ast.h \
		userspace/bin/holyd/src/ast/type_syntax.c userspace/bin/holyd/src/ast/type_syntax.h \
		tests/holyd_ffi_stub.c userspace/bin/holyd/src/ffi.h \
		| $(HOST_TEST_DIR)
	$(HOST_CC) $(HOST_TEST_CFLAGS) -I userspace -I userspace/bin/holyd/src \
		tests/holyd_compiler_test.c userspace/bin/holyd/src/compiler.c \
		userspace/bin/holyd/src/runtime.c userspace/bin/holyd/src/ast/type_syntax.c \
		userspace/bin/holyd/src/eval.c userspace/bin/holyd/src/lexer/lexer.c \
		userspace/bin/holyd/src/parser/parser.c userspace/bin/holyd/src/ast/ast.c \
		tests/holyd_ffi_stub.c \
		-o $@

.PHONY: test test-host
test: test-host

test-host: $(HOST_TEST_BINS)
	@set -e; for test_bin in $(HOST_TEST_BINS); do \
		echo "==> $$test_bin"; \
		"$$test_bin"; \
	done
	$(call run_linux,e2fsck -fn build/tests/ext2-mutated.img)

# --- holyd for Windows ----------------------------------------------------
#
# HolyD builds as a native Windows program as well as into the image. That
# build is the submodule's own , it has a Makefile, and keeping a second
# source list here is exactly how the two would drift. This target is a
# convenience so it still runs from the top of the TOS tree.
.PHONY: holyd-win
holyd-win:
	$(MAKE) -C userspace/bin/holyd
	@echo "built userspace/bin/holyd/holyd.exe , run it from that directory:"
	@echo "  ./holyd.exe samples/gui.hd"

.PHONY: test-qemu-heavy test-heavy
test-qemu-heavy: build-x86_64
	wsl bash -lc "cd \$$(wslpath '$(CURDIR)') && \
		python3 tests/smp_async_spawn_test.py --cpus 4 --timeout 90 && \
		python3 tests/system_stress_test.py --cpus 4 --timeout 240 && \
		python3 tests/window_lifecycle_test.py --timeout 120 && \
		python3 tests/muse_liveness_test.py --timeout 90 && \
		python3 tests/fb_mapping_lifetime_test.py --timeout 90 && \
		python3 tests/virtio_resize_test.py --boot-timeout 90 && \
		python3 tests/deskelf_test.py --timeout 90 && \
		python3 tests/netmon_test.py --timeout 120 && \
		python3 tests/net_arp_test.py --timeout 120 && \
		python3 tests/net_ping_test.py --timeout 180 && \
		python3 tests/net_udp_test.py --timeout 120 && \
		python3 tests/winman_partial_repaint_test.py --timeout 90 && \
		python3 tests/winman_titlebar_double_click_test.py --timeout 90 && \
		python3 tests/path_lookup_test.py --timeout 90 && \
		python3 tests/ehci_test.py && \
		python3 tests/kernel_panic_test.py --timeout 90"

test-heavy: test-host test-qemu-heavy

clean_paths := build dist \
	$(iso_dir)/boot/kernel.bin \
	$(iso_dir)/boot/disk.img \
	$(iso_dir)/boot/grub/eltorito.img \
	$(iso_dir)/boot/grub/efi.img \
	$(iso_dir)/EFI

clean:
	$(call run_linux,rm -rf $(clean_paths))
	$(MAKE) -C userspace clean

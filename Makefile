rwildcard = $(foreach d,$(wildcard $1*),$(call rwildcard,$d/,$2) $(filter $(subst *,%,$2),$d))

# Kernel C sources and their headers live together under kernel/<subsystem>/.
# Architecture assembly lives under kernel/arch/x86_64/.
kernel_c_source_files := $(call rwildcard,kernel/,*.c)
kernel_c_object_files := $(patsubst kernel/%.c, build/kernel/%.o, $(kernel_c_source_files))

# AP trampoline is assembled flat (org 0x8000, real-mode start) — never elf64.
# Filter it out of the regular asm rule and build it via the explicit
# ap_trampoline.bin -> ap_trampoline.o pipeline below.
ap_trampoline_src := kernel/arch/x86_64/boot/ap_trampoline.asm
ap_trampoline_bin := build/kernel/arch/x86_64/boot/ap_trampoline.bin
ap_trampoline_obj := build/kernel/arch/x86_64/boot/ap_trampoline.o
kernel_asm_source_files := $(filter-out $(ap_trampoline_src), $(call rwildcard,kernel/,*.asm))
kernel_asm_object_files := $(patsubst kernel/%.asm, build/kernel/%.o, $(kernel_asm_source_files))

kernel_object_files := $(kernel_c_object_files) $(kernel_asm_object_files) $(ap_trampoline_obj)

# -MMD -MP makes gcc emit a .d file per object listing the headers it pulled
# in. Without it an object depends only on its .c, so editing a header rebuilds
# nothing and the link silently reuses objects compiled against the old
# definitions — the resulting binary is a mix of both, which reads as "my fix
# had no effect".
kernel_c_flags := -I kernel -ffreestanding -mno-red-zone -mcmodel=kernel -fno-pic -fno-pie -MMD -MP

kernel_dep_files := $(kernel_c_object_files:.o=.d)

linker_script := boot/x86_64/linker.ld
iso_dir       := boot/x86_64/iso
kernel_bin    := dist/x86_64/kernel.bin
disk_img      := build/disk.img
USERSPACE_CLEAN ?= 0
BUILD_DOOM ?= 0

nvidia_firmware_files := $(wildcard rootfs/firmware/*.bin)
rootfs_payload_files := rootfs/readme.txt rootfs/cursor.bmp \
                        rootfs/music/beethoven.wav \
                        $(nvidia_firmware_files)

$(kernel_c_object_files): build/kernel/%.o : kernel/%.c
	mkdir -p $(dir $@) && \
	x86_64-elf-gcc -c $(kernel_c_flags) $(patsubst build/kernel/%.o, kernel/%.c, $@) -o $@

# Pull in the generated header dependencies. Leading '-' so a clean tree (no
# .d files yet) is not an error.
-include $(kernel_dep_files)

$(kernel_asm_object_files): build/kernel/%.o : kernel/%.asm
	mkdir -p $(dir $@) && \
	nasm -f elf64 $(patsubst build/kernel/%.o, kernel/%.asm, $@) -o $@

# AP trampoline: flat 16/32/64-bit blob, wrapped as an ELF .rodata symbol so
# the kernel can `memcpy` it to physical 0x8000 before INIT-SIPI-SIPI.
$(ap_trampoline_bin): $(ap_trampoline_src)
	mkdir -p $(dir $@) && nasm -f bin $< -o $@

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
	$(MAKE) -C userspace all BUILD_DOOM=$(BUILD_DOOM)

$(disk_img): userspace tools/create_disk.sh $(rootfs_payload_files)
	wsl bash -c "cd \$$(wslpath '$(CURDIR)') && bash tools/create_disk.sh"

# Link only — no disk image, no ISO. Useful for a quick compile check.
.PHONY: kernel
kernel: $(kernel_bin)

$(kernel_bin): $(kernel_object_files) $(linker_script)
	mkdir -p $(dir $@) && \
	x86_64-elf-ld -z max-page-size=0x1000 -o $@ -T $(linker_script) $(kernel_object_files)

.PHONY: build-x86_64
build-x86_64: $(kernel_bin) $(disk_img)
	mkdir -p $(iso_dir)/boot/grub && \
	cp $(kernel_bin) $(iso_dir)/boot/kernel.bin && \
	cp $(disk_img)   $(iso_dir)/boot/disk.img && \
	wsl bash -c "\
		cd \$$(wslpath '$(CURDIR)') && \
		grub-mkimage \
			-d /usr/lib/grub/i386-pc \
			-O i386-pc \
			-o /tmp/core.img \
			-p /boot/grub \
			biosdisk iso9660 normal multiboot2 all_video && \
		cat /usr/lib/grub/i386-pc/cdboot.img /tmp/core.img \
			> $(iso_dir)/boot/grub/eltorito.img && \
		xorriso -as mkisofs \
			-b boot/grub/eltorito.img \
			-no-emul-boot \
			-boot-load-size 4 \
			-boot-info-table \
			-o dist/x86_64/kernel.iso \
			$(iso_dir) \
	"

# --- host + QEMU regression suites -------------------------------------

HOST_CC ?= gcc
HOST_TEST_CFLAGS := -std=c11 -O2 -Wall -Wextra
HOST_TEST_DIR := build/tests
HOST_TEST_BINS := \
	$(HOST_TEST_DIR)/pmm_test.exe \
	$(HOST_TEST_DIR)/vmm_test.exe \
	$(HOST_TEST_DIR)/process_pml4_test.exe \
	$(HOST_TEST_DIR)/fat_directory_test.exe \
	$(HOST_TEST_DIR)/stdio_mode_test.exe \
	$(HOST_TEST_DIR)/bmp_decode_test.exe \
	$(HOST_TEST_DIR)/gfx_ui_test.exe \
	$(HOST_TEST_DIR)/fb_damage_test.exe

$(HOST_TEST_DIR):
	mkdir -p $@

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
		kernel/fs/fat.c kernel/fs/fat.h | $(HOST_TEST_DIR)
	$(HOST_CC) $(HOST_TEST_CFLAGS) -I kernel \
		tests/fat_directory_test.c kernel/fs/fat.c -o $@

$(HOST_TEST_DIR)/stdio_mode_test.exe: tests/stdio_mode_test.c \
		kernel/fs/stdio.c kernel/fs/fat.c kernel/fs/stdio.h kernel/fs/fat.h \
		| $(HOST_TEST_DIR)
	$(HOST_CC) $(HOST_TEST_CFLAGS) -I kernel tests/stdio_mode_test.c \
		kernel/fs/stdio.c kernel/fs/fat.c -o $@

$(HOST_TEST_DIR)/bmp_decode_test.exe: tests/bmp_decode_test.c \
		userspace/lib/bmp.c userspace/lib/bmp.h | $(HOST_TEST_DIR)
	$(HOST_CC) $(HOST_TEST_CFLAGS) -I userspace/lib \
		tests/bmp_decode_test.c userspace/lib/bmp.c -o $@

$(HOST_TEST_DIR)/gfx_ui_test.exe: tests/gfx_ui_test.c userspace/lib/gfx.c \
		userspace/lib/ui.c userspace/lib/bmp.c | $(HOST_TEST_DIR)
	$(HOST_CC) $(HOST_TEST_CFLAGS) -I userspace/lib tests/gfx_ui_test.c \
		userspace/lib/gfx.c userspace/lib/ui.c userspace/lib/bmp.c -o $@

$(HOST_TEST_DIR)/fb_damage_test.exe: tests/fb_damage_test.c | $(HOST_TEST_DIR)
	$(HOST_CC) $(HOST_TEST_CFLAGS) $< -o $@

.PHONY: test test-host
test: test-host

test-host: $(HOST_TEST_BINS)
	@set -e; for test_bin in $(HOST_TEST_BINS); do \
		echo "==> $$test_bin"; \
		"$$test_bin"; \
	done

.PHONY: test-qemu-heavy test-heavy
test-qemu-heavy: build-x86_64
	wsl bash -lc "cd \$$(wslpath '$(CURDIR)') && \
		python3 tests/smp_async_spawn_test.py --cpus 4 --timeout 90 && \
		python3 tests/system_stress_test.py --cpus 4 --timeout 240 && \
		python3 tests/window_lifecycle_test.py --timeout 120 && \
		python3 tests/fb_mapping_lifetime_test.py --timeout 90 && \
		python3 tests/virtio_resize_test.py --boot-timeout 90 && \
		python3 tests/deskelf_test.py --timeout 90 && \
		python3 tests/path_lookup_test.py --timeout 90 && \
		python3 tests/kernel_panic_test.py --timeout 90"

test-heavy: test-host test-qemu-heavy

clean:
	wsl bash -c "cd \$$(wslpath '$(CURDIR)') && rm -rf build dist $(iso_dir)/boot/kernel.bin $(iso_dir)/boot/disk.img $(iso_dir)/boot/grub/eltorito.img"
	$(MAKE) -C userspace clean

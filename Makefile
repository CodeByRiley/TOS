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

clean:
	wsl bash -c "cd \$$(wslpath '$(CURDIR)') && rm -rf build dist $(iso_dir)/boot/kernel.bin $(iso_dir)/boot/disk.img $(iso_dir)/boot/grub/eltorito.img"
	$(MAKE) -C userspace clean

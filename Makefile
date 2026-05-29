rwildcard = $(foreach d,$(wildcard $1*),$(call rwildcard,$d/,$2) $(filter $(subst *,%,$2),$d))

kernel_source_files := $(call rwildcard,src/impl/kernel/,*.c)
kernel_object_files := $(patsubst src/impl/kernel/%.c, build/kernel/%.o, $(kernel_source_files))
x86_64_c_source_files := $(call rwildcard,src/impl/x86_64/,*.c)
x86_64_c_object_files := $(patsubst src/impl/x86_64/%.c, build/x86_64/%.o, $(x86_64_c_source_files))
# AP trampoline is assembled flat (org 0x8000, real-mode start) — never elf64.
# Filter it out of the regular asm rule and build it via the explicit
# ap_trampoline.bin -> ap_trampoline.o pipeline below.
ap_trampoline_src := src/impl/x86_64/boot/ap_trampoline.asm
ap_trampoline_bin := build/x86_64/boot/ap_trampoline.bin
ap_trampoline_obj := build/x86_64/boot/ap_trampoline.o
x86_64_asm_source_files := $(filter-out $(ap_trampoline_src), $(call rwildcard,src/impl/x86_64/,*.asm))
x86_64_asm_object_files := $(patsubst src/impl/x86_64/%.asm, build/x86_64/%.o, $(x86_64_asm_source_files))
x86_64_object_files := $(x86_64_c_object_files) $(x86_64_asm_object_files) $(ap_trampoline_obj)
kernel_c_flags := -I src/intf -ffreestanding -mno-red-zone
rootfs_payload_files := disk_root/readme.txt disk_root/games/doom/doom.wad

$(kernel_object_files): build/kernel/%.o : src/impl/kernel/%.c
	mkdir -p $(dir $@) && \
	x86_64-elf-gcc -c $(kernel_c_flags) $(patsubst build/kernel/%.o, src/impl/kernel/%.c, $@) -o $@

$(x86_64_c_object_files): build/x86_64/%.o : src/impl/x86_64/%.c
	mkdir -p $(dir $@) && \
	x86_64-elf-gcc -c $(kernel_c_flags) $(patsubst build/x86_64/%.o, src/impl/x86_64/%.c, $@) -o $@

$(x86_64_asm_object_files): build/x86_64/%.o : src/impl/x86_64/%.asm
	mkdir -p $(dir $@) && \
	nasm -f elf64 $(patsubst build/x86_64/%.o, src/impl/x86_64/%.asm, $@) -o $@

# AP trampoline: flat 16/32/64-bit blob, wrapped as an ELF .rodata symbol so
# the kernel can `memcpy` it to physical 0x8000 before INIT-SIPI-SIPI.
$(ap_trampoline_bin): $(ap_trampoline_src)
	mkdir -p $(dir $@) && nasm -f bin $< -o $@

$(ap_trampoline_obj): $(ap_trampoline_bin)
	x86_64-elf-objcopy -I binary -O elf64-x86-64 -B i386 \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents \
		$< $@

.PHONY: userspace
userspace:
	$(MAKE) -C userspace clean
	$(MAKE) -C userspace all

disk.img: userspace create_disk.sh $(rootfs_payload_files)
	wsl bash -c "cd \$$(wslpath '$(CURDIR)') && bash create_disk.sh"

.PHONY: build-x86_64
build-x86_64: $(kernel_object_files) $(x86_64_object_files) disk.img
	mkdir -p dist/x86_64 && \
	mkdir -p targets/x86_64/iso/boot/grub && \
	x86_64-elf-ld -n -o dist/x86_64/kernel.bin -T targets/x86_64/linker.ld $(kernel_object_files) $(x86_64_object_files) && \
	cp dist/x86_64/kernel.bin targets/x86_64/iso/boot/kernel.bin && \
	cp disk.img               targets/x86_64/iso/boot/disk.img && \
	wsl bash -c "\
		cd \$$(wslpath '$(CURDIR)') && \
		grub-mkimage \
			-d /usr/lib/grub/i386-pc \
			-O i386-pc \
			-o /tmp/core.img \
			-p /boot/grub \
			biosdisk iso9660 normal multiboot2 all_video && \
		cat /usr/lib/grub/i386-pc/cdboot.img /tmp/core.img \
			> targets/x86_64/iso/boot/grub/eltorito.img && \
		xorriso -as mkisofs \
			-b boot/grub/eltorito.img \
			-no-emul-boot \
			-boot-load-size 4 \
			-boot-info-table \
			-o dist/x86_64/kernel.iso \
			targets/x86_64/iso \
	"

clean:
	wsl bash -c "cd \$$(wslpath '$(CURDIR)') && rm -rf build dist disk.img targets/x86_64/iso/boot/kernel.bin targets/x86_64/iso/boot/disk.img"
	$(MAKE) -C userspace clean

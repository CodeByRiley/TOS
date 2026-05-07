kernel_source_files := $(shell find src/impl/kernel -name *.c)
kernel_object_files := $(patsubst src/impl/kernel/%.c, build/kernel/%.o, $(kernel_source_files))
x86_64_c_source_files := $(shell find src/impl/x86_64 -name *.c)
x86_64_c_object_files := $(patsubst src/impl/x86_64/%.c, build/x86_64/%.o, $(x86_64_c_source_files))
x86_64_asm_source_files := $(shell find src/impl/x86_64 -name *.asm)
x86_64_asm_object_files := $(patsubst src/impl/x86_64/%.asm, build/x86_64/%.o, $(x86_64_asm_source_files))
x86_64_object_files := $(x86_64_c_object_files) $(x86_64_asm_object_files)
kernel_c_flags := -I src/intf -ffreestanding -mno-red-zone

$(kernel_object_files): build/kernel/%.o : src/impl/kernel/%.c
	mkdir -p $(dir $@) && \
	x86_64-elf-gcc -c $(kernel_c_flags) $(patsubst build/kernel/%.o, src/impl/kernel/%.c, $@) -o $@

$(x86_64_c_object_files): build/x86_64/%.o : src/impl/x86_64/%.c
	mkdir -p $(dir $@) && \
	x86_64-elf-gcc -c $(kernel_c_flags) $(patsubst build/x86_64/%.o, src/impl/x86_64/%.c, $@) -o $@

$(x86_64_asm_object_files): build/x86_64/%.o : src/impl/x86_64/%.asm
	mkdir -p $(dir $@) && \
	nasm -f elf64 $(patsubst build/x86_64/%.o, src/impl/x86_64/%.asm, $@) -o $@

.PHONY: build-x86_64
build-x86_64: $(kernel_object_files) $(x86_64_object_files)
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

 qemu-system-x86_64 ^
	-cdrom dist/x86_64/kernel.iso ^
	-cpu qemu64,+sse,+sse2,+sse4.1,+sse4.2,+sse4a,+ssse3 -smp 4 ^
	-vga virtio -usbdevice tablet -serial stdio -m 2048M

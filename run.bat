 qemu-system-x86_64 ^
 	-machine q35 ^
	-cdrom dist/x86_64/kernel.iso ^
  -accel tcg,thread=multi,tb-size=128 ^
	-cpu max -smp 4 ^
	-vga virtio -usbdevice tablet -serial stdio -m 49152M

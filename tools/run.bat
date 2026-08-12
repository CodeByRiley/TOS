@echo off
pushd "%~dp0.."
qemu-system-x86_64 ^
    -machine q35 ^
    -cdrom dist/x86_64/kernel.iso ^
    -accel tcg,thread=multi,tb-size=128 ^
    -cpu max -smp 4 ^
    -vga virtio -serial stdio -m 16384M ^
    -audiodev dsound,id=snd0 ^
    -device piix3-usb-uhci,id=uhci -device usb-tablet,bus=uhci.0 ^
    -device sb16,audiodev=snd0
popd

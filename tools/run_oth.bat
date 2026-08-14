
@echo off
pushd "%~dp0.."
qemu-system-x86_64 ^
    -machine q35 ^
    -cdrom dist/x86_64/kernel.iso ^
    -accel whpx,kernel-irqchip=on -cpu qemu64 -smp 4 ^
    -vga std -serial stdio -m 16384M ^
    -audiodev sdl,id=snd0 ^
    -device piix3-usb-uhci,id=uhci -device usb-tablet,bus=uhci.0 ^
    -device sb16,audiodev=snd0 ^
    -netdev user,id=n0,dhcpstart=10.0.2.30,hostfwd=tcp::2222-:22 ^
    -device e1000,netdev=n0 ^
    -object filter-dump,id=f0,netdev=n0,file=net.pcap
popd

@echo off
setlocal

pushd "%~dp0.." || exit /b 1

set "FS=fat"

set "KEY=%~1"
set "VALUE=%~2"

echo [DEBUG] key=[%KEY%] value=[%VALUE%]

if "%VALUE%"=="" (
    echo Missing value for argument: [%KEY%]
    popd
    endlocal
    exit /b 1
)

if /i "%KEY%"=="FS" (
    set "FS=%VALUE%"
) else (
    echo Invalid argument: [%KEY%]
    popd
    endlocal
    exit /b 1
)

echo [DEBUG] FS=[%FS%]

if /i "%FS%"=="fat" (
    set "DISK=build/disk-fat.img"
) else if /i "%FS%"=="ext2" (
    set "DISK=build/disk-ext2.img"
) else (
    echo Invalid filesystem: [%FS%]
    popd
    endlocal
    exit /b 1
)

echo [DEBUG] DISK=[%DISK%]

if not exist "%DISK%" (
    echo Disk image not found: [%DISK%]
    popd
    endlocal
    exit /b 1
)

qemu-system-x86_64 ^
    -machine q35 ^
    -cdrom dist/x86_64/kernel.iso ^
    -drive id=disk,file="%DISK%",if=none,format=raw ^
    -device ide-hd,drive=disk,bus=ide.0 ^
    -boot d ^
    -accel tcg,thread=multi,tb-size=128 ^
    -cpu max ^
    -smp 4 ^
    -vga virtio ^
    -serial stdio ^
    -m 16384M ^
    -audiodev sdl,id=snd0 ^
    -device piix3-usb-uhci,id=uhci ^
    -device usb-tablet,bus=uhci.0 ^
    -device sb16,audiodev=snd0 ^
    -netdev user,id=n0,dhcpstart=10.0.2.30,hostfwd=tcp::2222-:22,hostfwd=udp::5000-:5000 ^
    -device e1000,netdev=n0 ^
    -object filter-dump,id=f0,netdev=n0,file=net.pcap ^
    -rtc base=localtime

set "RESULT=%ERRORLEVEL%"

popd
endlocal
exit /b %RESULT%

REM -device e1000,netdev=n0 ^

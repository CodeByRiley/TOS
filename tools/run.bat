@echo off
setlocal

pushd "%~dp0.." || exit /b 1

set "FS=fat"
set "KEY=%~1"
set "VALUE=%~2"
if not defined QEMU_SHARE set "QEMU_SHARE=C:\Progra~1\qemu\share"
set "UEFI_CODE=%QEMU_SHARE%\edk2-x86_64-code.fd"
REM QEMU's variable-store template is architecture-neutral despite its name.
set "UEFI_VARS_SRC=%QEMU_SHARE%\edk2-i386-vars.fd"
set "UEFI_VARS=build\edk2-x86_64-vars.fd"

echo [DEBUG] UEFI_CODE=[%UEFI_CODE%]
echo [DEBUG] UEFI_VARS=[%UEFI_VARS%]

if not exist "%UEFI_CODE%" (
    echo [Error] UEFI firmware not found: [%UEFI_CODE%]
    popd
    endlocal
    exit /b 1
)

if not exist "%UEFI_VARS_SRC%" (
    echo [Error] UEFI variables template not found: [%UEFI_VARS_SRC%]
    popd
    endlocal
    exit /b 1
)

if exist "%UEFI_VARS%" (
	echo [Debug] Deleting stale UEFI vars
	del /F /Q "%UEFI_VARS%"
)
if not exist "%UEFI_VARS%" (
    copy /Y "%UEFI_VARS_SRC%" "%UEFI_VARS%" >nul
)

if not exist "%UEFI_VARS%" (
    copy /Y "%UEFI_VARS_SRC%" "%UEFI_VARS%" >nul
)

echo [DEBUG] key=[%KEY%] value=[%VALUE%]

if "%VALUE%"=="" (
    echo [Error] Missing value for argument: [%KEY%]
    popd
    endlocal
    exit /b 1
)

if /i "%KEY%"=="FS" (
    set "FS=%VALUE%"
) else (
    echo [Error] Invalid argument: [%KEY%]
    popd
    endlocal
    exit /b 1
)
if /i "%FS%"=="fat32" (
		set "DISK=build/disk-fat.img"
) else if /i "%FS%"=="fat" (
    set "DISK=build/disk-fat.img"
) else if /i "%FS%"=="ext2" (
    set "DISK=build/disk-ext2.img"
) else (
    echo [Error] Invalid filesystem: [%FS%]
    popd
    endlocal
    exit /b 1
)

if not exist "%DISK%" (
    echo [Error] Disk image not found: [%DISK%]
    popd
    endlocal
    exit /b 1
)


echo [DEBUG] FS=[%FS%]
echo [DEBUG] DISK=[%DISK%]

qemu-system-x86_64 ^
    -machine q35 ^
    -drive if=pflash,format=raw,readonly=on,file="%UEFI_CODE%" ^
    -drive if=pflash,format=raw,file="%UEFI_VARS%" ^
    -cdrom dist/x86_64/kernel.iso ^
    -drive id=disk,file="%DISK%",if=none,format=raw ^
    -device ide-hd,drive=disk,bus=ide.0 ^
    -drive id=data-disk,file="./build/data.img",if=none,format=raw ^
    -device ide-hd,drive=data-disk,bus=ide.1 ^
    -boot d ^
    -accel tcg,thread=multi,tb-size=128 ^
    -cpu max ^
    -smp 4 ^
    -vga virtio ^
    -serial stdio ^
    -m 512M ^
    -device ich9-usb-ehci1,id=ehci ^
    -device usb-tablet,bus=ehci.0 ^
    -audiodev sdl,id=snd0 ^
    -device sb16,audiodev=snd0 ^
    -netdev user,id=n0,dhcpstart=10.0.2.30,hostfwd=tcp::2222-:22,hostfwd=udp::5000-:5000 ^
    -device e1000,netdev=n0 ^
    -object filter-dump,id=f0,netdev=n0,file=net.pcap ^
    -rtc base=localtime

set "RESULT=%ERRORLEVEL%"

popd
endlocal
exit /b %RESULT%

REM -vga virtio ^
REM -device piix3-usb-uhci,id=uhci ^
REM -device usb-tablet,bus=uhci.0 ^

@echo off
setlocal

pushd "%~dp0.." || exit /b 1

set "BUILD_ALL=0"
set "BUILD_DOOM=0"
set "BUILD_NETSURF=0"
set "FS=fat"

:parse_args
if "%~1"=="" goto args_done

for /f "tokens=1,* delims==" %%A in ("%~1") do (
    if /i "%%A"=="BUILD_ALL"    set "BUILD_ALL=%%B"
    if /i "%%A"=="BUILD_DOOM"   set "BUILD_DOOM=%%B"
    if /i "%%A"=="BUILD_NETSURF" set "BUILD_NETSURF=%%B"
    if /i "%%A"=="FS"           set "FS=%%B"
)

shift
goto parse_args

:args_done

if /i "%FS%"=="EXT2" (
    set "ROOTFS_TYPE=ext2"
) else if /i "%FS%"=="FAT32" (
    set "ROOTFS_TYPE=fat"
) else (
    echo Unsupported filesystem: %FS%
    popd
    endlocal
    exit /b 1
)

echo BUILD_ALL=%BUILD_ALL%
echo BUILD_DOOM=%BUILD_DOOM%
echo BUILD_NETSURF=%BUILD_NETSURF%
echo ROOTFS_TYPE=%ROOTFS_TYPE%

if "%BUILD_ALL%"=="1" (
    set "BUILD_DOOM=1"
    set "BUILD_NETSURF=1"
)

mingw32-make build-x86_64 ^
    BUILD_DOOM=%BUILD_DOOM% ^
    BUILD_NETSURF=%BUILD_NETSURF% ^
    ROOTFS_TYPE=%ROOTFS_TYPE%

set "RESULT=%ERRORLEVEL%"

popd
endlocal
exit /b %RESULT%

@echo off
setlocal

pushd "%~dp0.." || exit /b 1

set "BUILD_ALL=0"
set "BUILD_DOOM=0"
set "BUILD_NETSURF=0"
set "CLEAN=0"
set "USERSPACE_CLEAN=0"
set "FS=fat"
set "ROOTFS_TYPE=fat"

:parse_args
if "%~1"=="" goto args_done

set "KEY=%~1"
set "VALUE=%~2"

echo [DEBUG] key=[%KEY%] value=[%VALUE%]

if "%VALUE%"=="" (
    echo Missing value for argument: [%KEY%]
    popd
    endlocal
    exit /b 1
)

if /i "%KEY%"=="BUILD_ALL" (
    set "BUILD_ALL=%VALUE%"
) else if /i "%KEY%"=="BUILD_DOOM" (
    set "BUILD_DOOM=%VALUE%"
) else if /i "%KEY%"=="BUILD_NETSURF" (
    set "BUILD_NETSURF=%VALUE%"
) else if /i "%KEY%"=="CLEAN" (
    set "CLEAN=%VALUE%"
    set "USERSPACE_CLEAN=%VALUE%"
) else if /i "%KEY%"=="FS" (
    set "FS=%VALUE%"
) else (
    echo Unknown argument: [%KEY%]
    popd
    endlocal
    exit /b 1
)

shift
shift
goto parse_args

:args_done

if /i "%FS%"=="ext2" (
    set "ROOTFS_TYPE=ext2"
) else if /i "%FS%"=="fat32" (
    set "ROOTFS_TYPE=fat"
) else if /i "%FS%"=="fat" (
    set "ROOTFS_TYPE=fat"
) else (
    echo Unsupported filesystem type: [%FS%]
    echo Supported filesystem types: ext2, fat, fat32
    popd
    endlocal
    exit /b 1
)

if "%BUILD_ALL%"=="1" (
    set "BUILD_DOOM=1"
    set "BUILD_NETSURF=1"
)

echo BUILD_DOOM=%BUILD_DOOM%
echo BUILD_NETSURF=%BUILD_NETSURF%
echo USERSPACE_CLEAN=%USERSPACE_CLEAN%
echo ROOTFS_TYPE=%ROOTFS_TYPE%

mingw32-make build-x86_64 ^
    BUILD_DOOM=%BUILD_DOOM% ^
    BUILD_NETSURF=%BUILD_NETSURF% ^
    USERSPACE_CLEAN=%USERSPACE_CLEAN% ^
    ROOTFS_TYPE=%ROOTFS_TYPE%

set "RESULT=%ERRORLEVEL%"

popd
endlocal
exit /b %RESULT%

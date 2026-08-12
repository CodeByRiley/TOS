@echo off
setlocal

pushd "%~dp0.."

echo %1

if /I "%~1"=="BUILD_DOOM" (
    echo Building with Doom
    mingw32-make -j 12 build-x86_64 BUILD_DOOM=1 USERSPACE_CLEAN=1
) else (
    echo Building without Doom
    mingw32-make -j 12 build-x86_64 USERSPACE_CLEAN=1
)

popd
endlocal

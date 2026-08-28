@echo off
setlocal

pushd "%~dp0.."

echo %1

if /I "%~1"=="BUILD_DOOM" (
    echo Building with Doom
    mingw32-make -j12 build-x86_64 BUILD_DOOM=1
) else (
    mingw32-make -j12 build-x86_64
)

popd
endlocal

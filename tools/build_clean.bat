@echo off
pushd "%~dp0.."
mingw32-make -j 12 build-x86_64 USERSPACE_CLEAN=1
popd

@echo off
pushd "%~dp0.."
mingw32-make -j 12 build-x86_64
popd

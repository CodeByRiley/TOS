#!/usr/bin/env sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
src_name=${MUSL_SRC:-musl-1.2.6}
build_name=${MUSL_BUILD:-build-musl-tos}

cd "$script_dir"

if [ ! -d "$src_name" ]; then
    echo "missing $script_dir/$src_name" >&2
    echo "extract musl before running this script" >&2
    exit 1
fi

mkdir -p "$build_name"
cd "$build_name"

if [ ! -f config.mak ]; then
    "../$src_name/configure" \
        --target=x86_64 \
        --disable-shared \
        CC="${CC:-x86_64-elf-gcc}" \
        AR="${AR:-x86_64-elf-ar}" \
        RANLIB="${RANLIB:-x86_64-elf-ranlib}" \
        CFLAGS="${CFLAGS:--ffreestanding -fno-pie -mno-red-zone -static}"
fi

if [ "$#" -eq 0 ]; then
    set -- lib/libc.a lib/crt1.o lib/crti.o lib/crtn.o
fi

exec make -f "../$src_name/Makefile" -f ../tos-musl.mk "$@"

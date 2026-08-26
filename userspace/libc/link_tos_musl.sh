#!/usr/bin/env sh
set -eu

if [ "$#" -lt 2 ]; then
    echo "usage: $0 output.elf source-or-object..." >&2
    exit 1
fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
src_name=${MUSL_SRC:-musl-1.2.6}
build_name=${MUSL_BUILD:-build-musl-tos}
cc=${CC:-/c/elf-tools/bin/x86_64-elf-gcc}

out=$1
shift

src_dir="$script_dir/$src_name"
build_dir="$script_dir/$build_name"
ld_script="$script_dir/../lib/user.ld"

if [ ! -f "$build_dir/lib/libc.a" ]; then
    "$script_dir/build_tos_musl.sh"
fi

gcc_include=$("$cc" -print-file-name=include)

exec "$cc" \
    -nostdinc \
    -nostdlib \
    -static \
    -ffreestanding \
    -fno-pie \
    -no-pie \
    -mno-red-zone \
    -O2 \
    -std=gnu11 \
    -I "$build_dir/obj/include" \
    -I "$src_dir/arch/x86_64" \
    -I "$src_dir/arch/generic" \
    -I "$src_dir/include" \
    -isystem "$gcc_include" \
    -DTOS_USE_MUSL \
    -I "$script_dir/.." \
    -I "$script_dir/../lib" \
    -I "$script_dir/../include" \
    -I "$script_dir/../../kernel" \
    -Wl,-T,"$ld_script" \
    -Wl,-z,max-page-size=0x1000 \
    -Wl,-z,noexecstack \
    "$build_dir/lib/crt1.o" \
    "$build_dir/lib/crti.o" \
    "$@" \
    "$build_dir/lib/libc.a" \
    -lgcc \
    "$build_dir/lib/crtn.o" \
    -o "$out"

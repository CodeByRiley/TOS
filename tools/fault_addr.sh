#!/bin/sh

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

if [ -n "$1" ]; then
    ELF="$1"
else
    printf "ELF file: "
    read -r ELF
fi

if [ -n "$2" ]; then
    ADDRESS="$2"
else
    printf "Address (hex): "
    read -r ADDRESS
fi

# Convert a repository-relative path to an absolute path.
case "$ELF" in
    /*|[A-Za-z]:[\\/]*)
        ;;
    *)
        ELF="$ROOT_DIR/$ELF"
        ;;
esac

if [ ! -f "$ELF" ]; then
    echo "ELF file not found: $ELF" >&2
    exit 1
fi

case "$ADDRESS" in
    0x[0-9a-fA-F]*|0X[0-9a-fA-F]*)
        ;;
    *)
        echo "Invalid address: $ADDRESS" >&2
        exit 1
        ;;
esac

echo "ELF:     $ELF"
echo "Address: $ADDRESS"

exec x86_64-elf-gdb "$ELF" \
    -ex "set architecture i386:x86-64" \
    -ex "break *$ADDRESS" \
    -ex "x/16i $ADDRESS"

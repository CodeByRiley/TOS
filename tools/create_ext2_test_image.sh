#!/usr/bin/env bash
set -euo pipefail

image="${1:-build/tests/ext2-base.img}"
block_size="${2:-1024}"
features="${3:-filetype}"
mkdir -p "$(dirname "$image")"
dd if=/dev/zero of="$image" bs=1M count=16 status=none
mke2fs -q -t ext2 -F -b "$block_size" -I 256 -O "$features" \
    -d tests/fixtures/ext2_root "$image"

#!/usr/bin/env bash
set -euo pipefail

image="${1:-build/tests/ext2-base.img}"
mkdir -p "$(dirname "$image")"
dd if=/dev/zero of="$image" bs=1M count=16 status=none
mke2fs -q -t ext2 -F -b 1024 -I 256 \
    -d tests/fixtures/ext2_root "$image"

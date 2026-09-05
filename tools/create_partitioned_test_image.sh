#!/usr/bin/env bash
# tools/create_partitioned_test_image.sh -- a raw disk with a real MBR.
#
# Builds the case the kernel could not read before partition support: a disk
# whose sector 0 holds a partition table rather than a filesystem. Two
# partitions, so the test can tell "found the table" from "found one entry":
#
#   p1  ext2, at LBA 2048
#   p2  FAT16, after it
#
# Filesystems are made in separate files and dd'd into place, because building
# them in situ would need a loop device and this has to run unprivileged.
#
# usage: create_partitioned_test_image.sh <output.img>
set -euo pipefail

out="${1:?usage: create_partitioned_test_image.sh <output.img>}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

sector=512
p1_start=2048
p1_size=32768   # 16 MiB
p2_start=36864
p2_size=32768   # 16 MiB
total=73728     # 36 MiB, leaving slack past the last partition

rm -f "$out"
dd if=/dev/zero of="$out" bs=$sector count=$total status=none

# sfdisk edits a plain file happily; type 83 is Linux, 06 is FAT16.
sfdisk --no-reread --no-tell-kernel "$out" >/dev/null <<EOF
label: dos
unit: sectors
${out}1 : start=$p1_start, size=$p1_size, type=83
${out}2 : start=$p2_start, size=$p2_size, type=06
EOF

mke2fs -q -F -t ext2 -b 1024 "$work/p1.img" $((p1_size / 2))k
mkfs.fat -F 16 -C "$work/p2.img" $((p2_size / 2)) >/dev/null

dd if="$work/p1.img" of="$out" bs=$sector seek=$p1_start conv=notrunc status=none
dd if="$work/p2.img" of="$out" bs=$sector seek=$p2_start conv=notrunc status=none

echo "$out created (${total} sectors, ext2 at $p1_start, FAT16 at $p2_start)"

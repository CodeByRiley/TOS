#!/usr/bin/env bash
set -euo pipefail

# (host_path::fat_dst_name) pairs
payloads=(
	"disk_root/readme.txt::README.TXT"
	"disk_root/games/doom/doom.wad::DOOM1.WAD"
	"userspace/bin/hello/hello.elf::HELLO.ELF"
	"userspace/bin/ls/ls.elf::LS.ELF"
	"userspace/bin/cat/cat.elf::CAT.ELF"
	"userspace/bin/gfx/gfx.elf::GFX.ELF"
	"userspace/bin/doom/doom.elf::DOOM.ELF"
	"userspace/bin/sh/sh.elf::SH.ELF"
	"userspace/bin/shutdown/shutdown.elf::SHUTDOWN.ELF"
	"userspace/bin/reboot/reboot.elf::REBOOT.ELF"
)

if [[ -z "${DISK_SIZE_MIB:-}" ]]; then
	payload_bytes=0
	for entry in "${payloads[@]}"; do
		host_path="${entry%%::*}"
		if [[ ! -f "$host_path" ]]; then
			echo "missing rootfs payload: $host_path" >&2
			exit 1
		fi
		payload_bytes=$((payload_bytes + $(wc -c < "$host_path")))
	done

	mib=$((1024 * 1024))
	slack_bytes=$((4 * mib))
	DISK_SIZE_MIB=$(((payload_bytes + slack_bytes + mib - 1) / mib))
	if ((DISK_SIZE_MIB < 16)); then
		DISK_SIZE_MIB=16
	fi
fi

dd if=/dev/zero of=disk.img bs=1M count="$DISK_SIZE_MIB" status=none
mkfs.fat -F 16 disk.img >/dev/null

for entry in "${payloads[@]}"; do
	host_path="${entry%%::*}"
	fat_name="${entry##*::}"
	mcopy -i disk.img "$host_path" "::${fat_name}"
done

echo "disk.img created (${DISK_SIZE_MIB} MiB)"

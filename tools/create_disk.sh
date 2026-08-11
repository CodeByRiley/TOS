#!/usr/bin/env bash
set -euo pipefail

# Run from the repository root; every path below is repo-relative.
cd "$(dirname "$0")/.."

# Output image. Lives in build/ so `make clean` takes it with everything else.
IMG="${IMG:-build/disk.img}"
mkdir -p "$(dirname "$IMG")"

# (host_path::fat_dst_name) pairs
payloads=(
	"rootfs/readme.txt::README.TXT"
	"rootfs/games/doom/doom.wad::DOOM1.WAD"
	"userspace/bin/hello/hello.elf::HELLO.ELF"
	"userspace/bin/ls/ls.elf::LS.ELF"
	"userspace/bin/cat/cat.elf::CAT.ELF"
	"userspace/bin/gfx/gfx.elf::GFX.ELF"
	"userspace/bin/doom/doom.elf::DOOM.ELF"
	"userspace/bin/sh/sh.elf::SH.ELF"
	"userspace/bin/shutdown/shutdown.elf::SHUTDOWN.ELF"
	"userspace/bin/reboot/reboot.elf::REBOOT.ELF"
	"userspace/bin/pkill/pkill.elf::PKILL.ELF"
	"userspace/bin/plist/plist.elf::PLIST.ELF"
	"userspace/bin/fdchild/fdchild.elf::FDCHILD.ELF"
	"userspace/bin/mtest/mtest.elf::MTEST.ELF"
	"userspace/bin/vmtest/vmtest.elf::VMTEST.ELF"
	"userspace/bin/pe_test/pe_test.exe::PE_TEST.EXE"
	"userspace/bin/winman/winman.elf::WINMAN.ELF"
	"userspace/bin/btop/btop.elf::BTOP.ELF"
)

# NVIDIA's upstream names exceed FAT 8.3. Keep the source names on the host
# and expose deterministic short names to the kernel. Firmware is optional:
# QEMU and non-NVIDIA systems continue to build the same root filesystem.
optional_payloads=(
	"rootfs/firmware/gsp_ga10x.bin::GSPGA10X.BIN"
	"rootfs/firmware/gsp_tu10x.bin::GSPTU10X.BIN"
	"rootfs/firmware/ucodes_ga10x.bin::UCGA10X.BIN"
	"rootfs/firmware/ucodes_tu10x.bin::UCTU10X.BIN"
)

for entry in "${optional_payloads[@]}"; do
	host_path="${entry%%::*}"
	if [[ -f "$host_path" ]]; then
		payloads+=("$entry")
	fi
done

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

dd if=/dev/zero of="$IMG" bs=1M count="$DISK_SIZE_MIB" status=none
mkfs.fat -F 16 "$IMG" >/dev/null

ensure_fat_parent_dirs() {
	local fat_path="${1#/}"
	local parent="${fat_path%/*}"
	if [[ "$parent" == "$fat_path" ]]; then
		return
	fi

	local current=""
	IFS='/' read -r -a components <<< "$parent"
	for component in "${components[@]}"; do
		[[ -z "$component" ]] && continue
		if [[ -z "$current" ]]; then
			current="$component"
		else
			current="$current/$component"
		fi
		if ! mdir -i "$IMG" "::${current}" >/dev/null 2>&1; then
			mmd -i "$IMG" "::${current}"
		fi
	done
}

for entry in "${payloads[@]}"; do
	host_path="${entry%%::*}"
	fat_name="${entry##*::}"
	ensure_fat_parent_dirs "$fat_name"
	mcopy -i "$IMG" "$host_path" "::${fat_name}"
done

echo "$IMG created (${DISK_SIZE_MIB} MiB)"

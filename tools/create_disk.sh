#!/usr/bin/env bash
set -euo pipefail

# Run from the repository root; every path below is repo-relative.
cd "$(dirname "$0")/.."

# Output image. Lives in build/ so `make clean` takes it with everything else.
IMG="${IMG:-build/disk.img}"
mkdir -p "$(dirname "$IMG")"

# (host_path::fat_dst_path) pairs. The volume is FAT32 with VFAT long
# names, so destinations are the real names — no 8.3 mangling, and no
# second spelling to keep in sync with the kernel's hardcoded paths.
payloads=(
	"rootfs/readme.txt::readme.txt"
	"rootfs/cursor.bmp::cursor.bmp"
	"rootfs/music/beethoven.wav::music/beethoven.wav"
	"rootfs/system/fonts/SansDisplayStatic.ttf::system/fonts/sansdisplaystatic.ttf"
	"rootfs/system/fonts/SansDisplayVariable.ttf::system/fonts/sansdisplayvariable.ttf"
	"rootfs/system/wallpaper.bmp::system/wallpaper.bmp"
	"rootfs/system/icons/DOOM.bmp::system/icons/doom.bmp"
	"rootfs/system/icons/shelf.bmp::system/icons/shelf.bmp"
	"userspace/bin/hello/hello.elf::hello.elf"
	"userspace/bin/ls/ls.elf::ls.elf"
	"userspace/bin/cat/cat.elf::cat.elf"
	"userspace/bin/gfx/gfx.elf::gfx.elf"
	"userspace/bin/sh/sh.elf::sh.elf"
	"userspace/bin/shutdown/shutdown.elf::shutdown.elf"
	"userspace/bin/reboot/reboot.elf::reboot.elf"
	"userspace/bin/pkill/pkill.elf::pkill.elf"
	"userspace/bin/plist/plist.elf::plist.elf"
	"userspace/bin/fdchild/fdchild.elf::fdchild.elf"
	"userspace/bin/mtest/mtest.elf::mtest.elf"
	"userspace/bin/vmtest/vmtest.elf::vmtest.elf"
	"userspace/bin/uidemo/uidemo.elf::uidemo.elf"
	"userspace/bin/pe_test/pe_test.exe::pe_test.exe"
	"userspace/bin/hello/hello.exe::hello.exe"
	"userspace/bin/ls/ls.exe::ls.exe"
	"userspace/bin/winman/winman.elf::winman.elf"
	"userspace/bin/btop/btop.elf::btop.elf"
	"userspace/bin/thread/thread.elf::thread.elf"
)

# Firmware and DOOM are optional: QEMU and non-NVIDIA systems continue to
# build the same root filesystem without them.
optional_payloads=(
	"rootfs/games/doom/doom.wad::games/doom/doom.wad"
	"userspace/bin/doom/doom.elf::games/doom/doom.elf"
	"rootfs/firmware/gsp_ga10x.bin::firmware/gsp_ga10x.bin"
	"rootfs/firmware/gsp_tu10x.bin::firmware/gsp_tu10x.bin"
	"rootfs/firmware/ucodes_ga10x.bin::firmware/ucodes_ga10x.bin"
	"rootfs/firmware/ucodes_tu10x.bin::firmware/ucodes_tu10x.bin"
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
	# FAT32 is only FAT32 above 65525 clusters -- below that the driver
	# reads the geometry as FAT16 and refuses the volume, because a real
	# FAT16 needs the fixed root table this image would not have. At one
	# 512-byte sector per cluster that floor is 32 MiB; 64 gives room.
	if ((DISK_SIZE_MIB < 64)); then
		DISK_SIZE_MIB=64
	fi
fi

dd if=/dev/zero of="$IMG" bs=1M count="$DISK_SIZE_MIB" status=none
# -s 1 keeps one sector per cluster so the cluster count stays well clear
# of the FAT32 floor no matter how small the payload set gets.
mkfs.fat -F 32 -s 1 "$IMG" >/dev/null

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

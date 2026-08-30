#!/usr/bin/env bash
set -euo pipefail

# Run from the repository root; every path below is repo-relative.
cd "$(dirname "$0")/.."

# FAT remains the default, while TOS_ROOTFS_TYPE=ext2 builds the same payload
# into an ext2 image. Check only the tools needed by the selected backend.
ROOTFS_TYPE="${TOS_ROOTFS_TYPE:-fat}"
case "$ROOTFS_TYPE" in
	fat) required_tools=(dd mkfs.fat mmd mdir mcopy) ;;
	ext2) required_tools=(dd mkfs mktemp cp) ;;
	*)
		echo "create_disk: unsupported filesystem '$ROOTFS_TYPE'" >&2
		exit 1
		;;
esac
for tool in "${required_tools[@]}"; do
	command -v "$tool" >/dev/null || {
		echo "create_disk: $tool not found on PATH" >&2
		echo "  Debian/Ubuntu: sudo apt install dosfstools mtools" >&2
		echo "  Fedora:        sudo dnf install dosfstools mtools" >&2
		echo "  Arch:          sudo pacman -S dosfstools mtools" >&2
		echo "  On Windows this step is meant to run inside WSL; see README." >&2
		exit 1
	}
done

MIN_SIZE=64

# Output image. Lives in build/ so `make clean` takes it with everything else.
IMG="${IMG:-build/disk.img}"
mkdir -p "$(dirname "$IMG")"

# (host_path::destination_path) pairs shared by both image formats.
payloads=(
	"rootfs/readme.txt::readme.txt"

	# The .hd scripts come from the HolyD submodule, which is their only
	# home now , edit them in userspace/bin/holyd and commit there.
	#
	# The split between the two directories is load-bearing: `holyd --test`
	# runs every .hd under holyd/tests and waits for each to finish, so a
	# script that waits on a person or on the network would hang the run.
	# Those are samples, and get run by path: holyd holyd/samples/gui.hd
	"userspace/bin/holyd/tests/array.hd::holyd/tests/array.hd"
	"userspace/bin/holyd/tests/conditionals.hd::holyd/tests/conditionals.hd"
	"userspace/bin/holyd/tests/strings.hd::holyd/tests/strings.hd"
	"userspace/bin/holyd/tests/hello.hd::holyd/tests/hello.hd"
	"userspace/bin/holyd/tests/math.hd::holyd/tests/math.hd"
	"userspace/bin/holyd/tests/functions.hd::holyd/tests/functions.hd"
	"userspace/bin/holyd/tests/no_semis.hd::holyd/tests/no_semis.hd"
	"userspace/bin/holyd/tests/holyc_d_style.hd::holyd/tests/holyc_d_style.hd"
	"userspace/bin/holyd/samples/window.hd::holyd/samples/window.hd"
	"userspace/bin/holyd/samples/gui.hd::holyd/samples/gui.hd"
	"userspace/bin/holyd/samples/net.hd::holyd/samples/net.hd"

	"rootfs/system/fonts/SansDisplayStatic.ttf::system/fonts/sansdisplaystatic.ttf"
	"rootfs/system/fonts/SansDisplayVariable.ttf::system/fonts/sansdisplayvariable.ttf"
	"rootfs/system/wallpaper.bmp::system/wallpaper.bmp"
	"rootfs/system/icons/icon.bmp::system/icons/icon.bmp"
	"rootfs/system/icons/DOOM.bmp::system/icons/doom.bmp"
	"rootfs/system/icons/shelf.bmp::system/icons/shelf.bmp"
	"rootfs/system/icons/cursor.bmp::system/icons/cursor.bmp"

# User Apps
	"userspace/bin/hello/hello.elf::usr/bin/hello.elf"
	"userspace/bin/gfx/gfx.elf::usr/bin/gfx.elf"
	"userspace/bin/fdchild/fdchild.elf::usr/bin/fdchild.elf"
	"userspace/bin/mtest/mtest.elf::usr/bin/mtest.elf"
	"userspace/bin/vmtest/vmtest.elf::usr/bin/vmtest.elf"
	"userspace/bin/uidemo/uidemo.elf::usr/bin/uidemo.elf"
	"userspace/bin/pe_test/pe_test.exe::usr/bin/pe_test.exe"
	"userspace/bin/hello/hello.exe::usr/bin/hello.exe"
	"userspace/bin/ls/ls.exe::usr/bin/ls.exe"
	"userspace/bin/btop/btop.elf::usr/bin/btop.elf"
	"userspace/bin/thread/thread.elf::usr/bin/thread.elf"
	"userspace/bin/deskelf/deskelf.elf::usr/bin/deskelf.elf"
	"userspace/bin/stress/stress.elf::usr/bin/stress.elf"
	"userspace/bin/stress_peer/stress_peer.elf::usr/bin/stress_peer.elf"
	"userspace/bin/audiotest/audiotest.elf::usr/bin/audiotest.elf"
	"userspace/bin/muse/muse.elf::usr/bin/muse.elf"
	"userspace/bin/mmaptest/mmaptest.elf::usr/bin/mmaptest.elf"
	"userspace/bin/faulter/faulter.elf::usr/bin/faulter.elf"
	"userspace/bin/netmon/netmon.elf::usr/bin/netmon.elf"
	"userspace/bin/ping/ping.elf::usr/bin/ping.elf"
	"userspace/bin/udpecho/udpecho.elf::usr/bin/udpecho.elf"

# System Apps
	"userspace/bin/shutdown/shutdown.elf::system/bin/shutdown.elf"
	"userspace/bin/reboot/reboot.elf::system/bin/reboot.elf"
	"userspace/bin/pkill/pkill.elf::system/bin/pkill.elf"
	"userspace/bin/winman/winman.elf::system/bin/winman.elf"
	"userspace/bin/plist/plist.elf::system/bin/plist.elf"
	"userspace/bin/holyd.elf::system/bin/holyd.elf"
	"userspace/bin/notepad/notepad.elf::system/bin/notepad.elf"
	"userspace/bin/sh/sh.elf::system/bin/sh.elf"
	"userspace/bin/ls/ls.elf::system/bin/ls.elf"
	"userspace/bin/cat/cat.elf::system/bin/cat.elf"
	"userspace/bin/tree/tree.elf::system/bin/tree.elf"
)

# Optional payloads: anything gitignored, unbuilt, or hardware-specific.
# A clean checkout must still produce a bootable image without them,
# which is the P0 item in TODO.txt.
optional_payloads=(
	# *.wav is gitignored, so a fresh clone has no way to produce this.
	"rootfs/music/beethoven.wav::music/beethoven.wav"
	"userspace/bin/netsurf/netsurf.elf::usr/bin/netsurf.elf"
	"userspace/netsurf_compat/generated/Messages::res/netsurf/Messages"
	"userspace/bin/netsurf/netsurf/frontends/framebuffer/res/adblock.css::res/netsurf/adblock.css"
	"userspace/bin/netsurf/netsurf/frontends/framebuffer/res/credits.html::res/netsurf/credits.html"
	"userspace/bin/netsurf/netsurf/frontends/framebuffer/res/default.css::res/netsurf/default.css"
	"userspace/bin/netsurf/netsurf/frontends/framebuffer/res/favicon.png::res/netsurf/favicon.png"
	"userspace/bin/netsurf/netsurf/frontends/framebuffer/res/internal.css::res/netsurf/internal.css"
	"userspace/bin/netsurf/netsurf/frontends/framebuffer/res/licence.html::res/netsurf/licence.html"
	"userspace/bin/netsurf/netsurf/frontends/framebuffer/res/netsurf.png::res/netsurf/netsurf.png"
	"userspace/bin/netsurf/netsurf/frontends/framebuffer/res/quirks.css::res/netsurf/quirks.css"
	"userspace/bin/netsurf/netsurf/frontends/framebuffer/res/welcome.html::res/netsurf/welcome.html"
	"userspace/bin/netsurf/netsurf/resources/icons/content.png::res/netsurf/icons/content.png"
	"userspace/bin/netsurf/netsurf/resources/icons/directory.png::res/netsurf/icons/directory.png"
	"userspace/libc/build-musl-tos/muslhello.elf::usr/bin/muslhello.elf"
	"userspace/libc/build-musl-tos/muslposix.elf::usr/bin/muslposix.elf"
	"rootfs/games/doom/doom.wad::games/doom/doom.wad"
	"userspace/bin/doom/doom.elf::usr/bin/doom.elf"
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
	if ((DISK_SIZE_MIB < MIN_SIZE)); then
		DISK_SIZE_MIB=$MIN_SIZE
	fi
fi

dd if=/dev/zero of="$IMG" bs=1M count="$DISK_SIZE_MIB" status=none

if [[ "$ROOTFS_TYPE" == "ext2" ]]; then
	staging="$(mktemp -d)"
	trap 'rm -rf -- "$staging"' EXIT
	mkdir -p "$staging/bin" "$staging/usr/bin" "$staging/usr/local/bin"
	for entry in "${payloads[@]}"; do
		host_path="${entry%%::*}"
		destination="${entry##*::}"
		mkdir -p "$staging/$(dirname "$destination")"
		cp "$host_path" "$staging/$destination"
	done
	# Classic ext2 (no journal) with 1 KiB blocks. The backend also accepts
	# the revision-0 128-byte inode layout, but modern tools prefer 256.
	mke2fs -q -t ext2 -F -b 1024 -I 256 -d "$staging" "$IMG"
	echo "$IMG created (${DISK_SIZE_MIB} MiB ext2)"
	exit 0
fi

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

# Keep the standard executable hierarchy present even when a directory has
# no packaged files yet. /usr/local/bin is intentionally reserved for tools
# installed after the base image is built.
for fat_dir in bin usr usr/bin usr/local usr/local/bin; do
	if ! mdir -i "$IMG" "::${fat_dir}" >/dev/null 2>&1; then
		mmd -i "$IMG" "::${fat_dir}"
	fi
done

for entry in "${payloads[@]}"; do
	host_path="${entry%%::*}"
	fat_name="${entry##*::}"
	ensure_fat_parent_dirs "$fat_name"
	mcopy -i "$IMG" "$host_path" "::${fat_name}"
done

echo "$IMG created (${DISK_SIZE_MIB} MiB FAT32)"

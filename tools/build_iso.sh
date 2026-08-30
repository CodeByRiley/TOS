#!/usr/bin/env bash
# tools/build_iso.sh — assemble the bootable ISO from kernel.bin + disk.img.
# Invoked by the Makefile (through WSL on Windows), but self-locating: safe to
# run directly from the project root when iterating on just this step.
set -euo pipefail

cd "$(dirname "$0")/.."

iso_dir=boot/x86_64/iso
kernel=dist/x86_64/kernel.bin
disk="${DISK_IMAGE:-build/disk.img}"
out_iso=dist/x86_64/kernel.iso

# Kept next to the other build output rather than in /tmp: a GRUB built for
# Windows cannot open a POSIX path like /tmp/core.img, and a repo-relative
# path works for every grub-mkimage regardless of which world it came from.
core_img=build/core.img

die() { printf 'build_iso: %s\n' "$@" >&2; exit 1; }

# --- fail fast, with messages that name the fix --------------------------
for tool in grub-mkimage xorriso; do
    command -v "$tool" >/dev/null || die \
        "$tool not found on PATH" \
        "  Debian/Ubuntu: sudo apt install grub-pc-bin grub-common xorriso" \
        "  Fedora:        sudo dnf install grub2-pc-modules grub2-tools xorriso" \
        "  Arch:          sudo pacman -S grub libisoburn" \
        "  On Windows, run the build so this step lands in WSL (see README);" \
        "  MSYS2 packages neither xorriso nor a usable i386-pc GRUB."
done

# --- locate the i386-pc GRUB modules -------------------------------------
# There is no portable answer here: Debian and Arch use /usr/lib/grub/i386-pc,
# a from-source install uses /usr/local/lib, and the prebuilt Windows bundle
# keeps i386-pc next to the executables. Try them all, and let GRUB_DIR win.
grub_candidates=()
[ -n "${GRUB_DIR:-}" ] && grub_candidates+=("$GRUB_DIR")
grub_candidates+=(
    /usr/lib/grub/i386-pc
    /usr/lib/grub2/i386-pc
    /usr/local/lib/grub/i386-pc
    /usr/share/grub/i386-pc
)
grub_bindir="$(dirname "$(command -v grub-mkimage)")"
grub_candidates+=(
    "$grub_bindir/../lib/grub/i386-pc"
    "$grub_bindir/i386-pc"
)

grub_dir=""
for candidate in "${grub_candidates[@]}"; do
    # moddep.lst is what -d reads; cdboot.img is what the El Torito image
    # starts with. A directory holding one but not the other is the wrong one.
    if [ -f "$candidate/moddep.lst" ] && [ -f "$candidate/cdboot.img" ]; then
        grub_dir="$candidate"
        break
    fi
done

if [ -z "$grub_dir" ]; then
    die "no i386-pc GRUB module directory found. Looked in:" \
        "$(printf '  %s\n' "${grub_candidates[@]}")" \
        "  A directory qualifies only if it has both moddep.lst and cdboot.img." \
        "  Install the BIOS (not EFI) GRUB modules, or point GRUB_DIR at them:" \
        "    GRUB_DIR=/path/to/grub/i386-pc bash tools/build_iso.sh"
fi

[ -f "$iso_dir/boot/grub/grub.cfg" ] || die "$iso_dir/boot/grub/grub.cfg is missing"

for input in "$kernel" "$disk"; do
    [ -f "$input" ] || die "$input does not exist yet , run the build first"
done

# --- stage the ISO tree --------------------------------------------------
mkdir -p "$iso_dir/boot/grub" dist/x86_64 "$(dirname "$core_img")"
cp "$kernel" "$iso_dir/boot/kernel.bin"
cp "$disk"   "$iso_dir/boot/disk.img"

echo "Project directory: $(pwd)"
echo "GRUB directory:    $grub_dir"
echo "grub-mkimage:      $(command -v grub-mkimage) ($(grub-mkimage --version))"

# --- grub eltorito boot image --------------------------------------------
rm -f "$core_img"
if ! grub-mkimage \
        -d "$grub_dir" \
        -O i386-pc \
        -o "$core_img" \
        -p /boot/grub \
        biosdisk iso9660 normal multiboot2 all_video
then
    die "grub-mkimage failed." \
        "  grub-mkimage and the modules in $grub_dir must come from the same" \
        "  GRUB install. A mismatch shows up as errors about moddep.lst or as" \
        "  \"kernel.img is miscompiled: its start address is 0x0\"."
fi

[ -s "$core_img" ] || die "grub-mkimage produced an empty $core_img"

cat "$grub_dir/cdboot.img" "$core_img" > "$iso_dir/boot/grub/eltorito.img"
[ -s "$iso_dir/boot/grub/eltorito.img" ] || die "empty eltorito.img"
rm -f "$core_img"

# --- the ISO ---------------------------------------------------------------
xorriso -as mkisofs \
    -b boot/grub/eltorito.img \
    -no-emul-boot \
    -boot-load-size 4 \
    -boot-info-table \
    -o "$out_iso" \
    "$iso_dir"

echo "==> $out_iso"

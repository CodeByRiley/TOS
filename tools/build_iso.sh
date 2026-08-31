#!/usr/bin/env bash
# tools/build_iso.sh — assemble a hybrid UEFI + Legacy BIOS bootable ISO.
set -euo pipefail

cd "$(dirname "$0")/.."

iso_dir=boot/x86_64/iso
kernel=dist/x86_64/kernel.bin
disk="${DISK_IMAGE:-build/disk.img}"
out_iso=dist/x86_64/kernel.iso

core_img=build/core.img
efi_img=build/efi.img

die() { printf 'build_iso: %s\n' "$@" >&2; exit 1; }

# fail fast, with messages that name required dependencies
for tool in grub-mkimage grub-mkstandalone xorriso mformat mmd mcopy; do
    command -v "$tool" >/dev/null || die \
        "$tool not found on PATH" \
        "  Debian/Ubuntu: sudo apt install grub-pc-bin grub-efi-amd64-bin grub-common xorriso mtools" \
        "  Fedora:        sudo dnf install grub2-pc-modules grub2-efi-x64-modules grub2-tools xorriso mtools" \
        "  Arch:          sudo pacman -S grub libisoburn mtools"
done

# locate GRUB modules
grub_candidates=()
[ -n "${GRUB_BIOS_DIR:-}" ] && grub_candidates+=("$GRUB_BIOS_DIR")
# Keep the old override working for existing developer setups.
[ -n "${GRUB_DIR:-}" ] && grub_candidates+=("$GRUB_DIR")
grub_candidates+=(
    /usr/lib/grub/i386-pc
    /usr/lib/grub2/i386-pc
    /usr/local/lib/grub/i386-pc
    /usr/share/grub/i386-pc
)

grub_bios_dir=""
for candidate in "${grub_candidates[@]}"; do
    if [ -f "$candidate/moddep.lst" ] && [ -f "$candidate/cdboot.img" ]; then
        grub_bios_dir="$candidate"
        break
    fi
done

[ -n "$grub_bios_dir" ] || die \
    "no i386-pc GRUB module directory found" \
    "  Install grub-pc-bin or set GRUB_BIOS_DIR=/path/to/grub/i386-pc."

grub_efi_candidates=()
[ -n "${GRUB_EFI_DIR:-}" ] && grub_efi_candidates+=("$GRUB_EFI_DIR")
grub_efi_candidates+=(
    /usr/lib/grub/x86_64-efi
    /usr/lib/grub2/x86_64-efi
    /usr/local/lib/grub/x86_64-efi
    /usr/share/grub/x86_64-efi
)
grub_efi_dir=""
for candidate in "${grub_efi_candidates[@]}"; do
    if [ -f "$candidate/moddep.lst" ]; then
        grub_efi_dir="$candidate"
        break
    fi
done

[ -n "$grub_efi_dir" ] || die \
    "no x86_64-efi GRUB module directory found" \
    "  Install grub-efi-amd64-bin or set GRUB_EFI_DIR=/path/to/grub/x86_64-efi."

[ -f "$iso_dir/boot/grub/grub.cfg" ] || die "$iso_dir/boot/grub/grub.cfg is missing"

for input in "$kernel" "$disk"; do
    [ -f "$input" ] || die "$input does not exist yet, run the build first"
done

# stage the ISO tree
mkdir -p "$iso_dir/boot/grub" "$iso_dir/EFI/BOOT" dist/x86_64 "$(dirname "$core_img")"
cp "$kernel" "$iso_dir/boot/kernel.bin"
cp "$disk"   "$iso_dir/boot/disk.img"

echo "Project directory: $(pwd)"
echo "GRUB BIOS dir:     $grub_bios_dir"
echo "GRUB EFI dir:      $grub_efi_dir"

# Legacy BIOS Eltorito Boot Image
rm -f "$core_img"
grub-mkimage \
    -d "$grub_bios_dir" \
    -O i386-pc \
    -o "$core_img" \
    -p /boot/grub \
    biosdisk iso9660 normal search search_fs_file multiboot2 all_video

[ -s "$core_img" ] || die "grub-mkimage produced an empty BIOS core image"

cat "$grub_bios_dir/cdboot.img" "$core_img" > "$iso_dir/boot/grub/eltorito.img"
rm -f "$core_img"

# BOOTX64.EFI is the removable-media fallback filename mandated by UEFI.
# Embedding grub.cfg lets GRUB start without depending on firmware-specific
# access to the ISO filesystem.  The config then searches for the ISO itself.
grub-mkstandalone \
    -d "$grub_efi_dir" \
    -O x86_64-efi \
    -o "$iso_dir/EFI/BOOT/BOOTX64.EFI" \
    "boot/grub/grub.cfg=$iso_dir/boot/grub/grub.cfg"

# Pack the loader into an 8 MiB FAT image for the UEFI El Torito entry.  A
# standalone GRUB can exceed the traditional 1.44 MiB floppy image size.
rm -f "$efi_img"
truncate -s 8M "$efi_img"
mformat -i "$efi_img" ::
mmd -i "$efi_img" ::/EFI ::/EFI/BOOT
mcopy -i "$efi_img" "$iso_dir/EFI/BOOT/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
cp "$efi_img" "$iso_dir/boot/grub/efi.img"

[ -s "$iso_dir/boot/grub/efi.img" ] || die "empty EFI El Torito image"

# Assemble a hybrid ISO with xorriso
xorriso -as mkisofs \
    -b boot/grub/eltorito.img \
    -no-emul-boot \
    -boot-load-size 4 \
    -boot-info-table \
    -eltorito-alt-boot \
    -e boot/grub/efi.img \
    -no-emul-boot \
    -o "$out_iso" \
    "$iso_dir"

echo "==> Successfully created dual-firmware UEFI/BIOS ISO: $out_iso"

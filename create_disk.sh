set -euo pipefail

dd if=/dev/zero of=disk.img bs=1M count=32
mkfs.fat -F 16 disk.img
mcopy -i disk.img README.TXT ::
mcopy -i disk.img ./games/doom/DOOM1.WAD ::

echo "disk.img created"

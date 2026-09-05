#!/usr/bin/env python3
"""Boot with two AHCI disks and mount the second one from the shell.

The root comes from the FAT disk on ide.0, which leaves the ext2 disk on
ide.1 published but unmounted -- exactly the case SYS_MOUNT exists for.

Assertions read the *kernel* log on the serial port, not shell output:
sh's stdout goes to the TTY once winman owns the display, so the mount and
umount syscalls logging their own result is what makes this observable.

Requires a built ISO, build/disk-fat.img and build/tests/ext2-base.img.
Only copies under build/tests are touched; never pass a physical host disk.
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import time
from pathlib import Path

from kernel_panic_test import Qmp, available_port, wait_for_text
from path_lookup_test import send_text


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--iso", default="dist/x86_64/kernel.iso")
    parser.add_argument("--timeout", type=float, default=180)
    args = parser.parse_args()

    iso = Path(args.iso).resolve()
    if not iso.exists():
        print(f"missing ISO: {iso}", file=sys.stderr)
        return 2

    tests = Path("build/tests")
    tests.mkdir(parents=True, exist_ok=True)
    root_image = (tests / "mount-root.img").resolve()
    extra_image = (tests / "mount-extra.img").resolve()
    shutil.copyfile("build/disk-fat.img", root_image)
    shutil.copyfile("build/tests/ext2-base.img", extra_image)

    log = (tests / "mount-syscall.log").resolve()
    log.write_bytes(b"")
    port = available_port()
    command = [
        args.qemu, "-cdrom", str(iso),
        # Two hard disks outrank the CD in SeaBIOS's default order, and
        # neither is bootable, so the machine would sit at "no bootable
        # device" with an empty serial log.
        "-boot", "d",
        "-serial", f"file:{log}", "-display", "none", "-vga", "virtio",
        "-qmp", f"tcp:127.0.0.1:{port},server=on,wait=off",
        "-no-reboot", "-m", "512M", "-smp", "2", "-machine", "q35",
        "-drive", f"if=none,id=rootdisk,file={root_image},format=raw",
        "-device", "ide-hd,drive=rootdisk,bus=ide.0",
        "-drive", f"if=none,id=extradisk,file={extra_image},format=raw",
        "-device", "ide-hd,drive=extradisk,bus=ide.1",
    ]

    proc = subprocess.Popen(command, stdin=subprocess.DEVNULL)
    qmp = None
    try:
        deadline = time.monotonic() + args.timeout

        def expect(text: str, why: str) -> None:
            if not wait_for_text(log, text, deadline):
                raise RuntimeError(
                    f"{why}: never saw {text!r}\n"
                    f"{log.read_text(errors='replace')}")

        # ide.0 holds FAT, so the root search stops there and never reaches
        # the ext2 disk. Assert it, or a swapped bus order would leave the
        # mount below testing an already-mounted volume.
        expect("rootfs: fat mounted from ahci0 at /", "root did not mount")
        expect("winman: ready", "desktop did not come up")

        qmp = Qmp(port, deadline)
        send_text(qmp, "mount ahci1 /mnt\n")
        expect("mount: ahci1 (ext2) at /mnt", "mount syscall did not mount")

        send_text(qmp, "mount -u /mnt\n")
        expect("umount: released /mnt", "umount syscall did not release")

        # Remounting is the point of leaving unclaimed AHCI adapters open: the
        # transport has to survive an unmount for a second mount to borrow it.
        # A different mountpoint keeps this from matching the first mount's
        # line if umount had silently left the volume attached.
        send_text(qmp, "mount ahci1 /mnt2 ext2\n")
        expect("mount: ahci1 (ext2) at /mnt2", "volume did not survive unmount")

        if "PANIC" in log.read_text(errors="replace"):
            raise RuntimeError(f"kernel panic\n{log.read_text(errors='replace')}")
    finally:
        if qmp:
            qmp.close()
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)

    print(f"mount: ext2 volume on ahci1 mounted, released, remounted; log: {log}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

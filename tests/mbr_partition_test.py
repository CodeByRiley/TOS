#!/usr/bin/env python3
"""Boot with an MBR-partitioned AHCI disk and mount a partition from it.

Before partition support this disk was unmountable: sector 0 holds a table,
not a filesystem, so every backend rejected it and the disk read as blank.
The FAT root on ide.0 keeps that failure from being masked by the disk simply
becoming the root.

Asserts on the kernel log over serial, not shell output: sh's stdout goes to
the TTY once winman owns the display, so the syscalls and the partition
scanner logging their own results is what makes this observable.

Requires a built ISO, build/disk-fat.img and build/tests/mbr-base.img
(tools/create_partitioned_test_image.sh). Only copies under build/tests are
touched; never pass a physical host disk.
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
    parser.add_argument("--timeout", type=float, default=200)
    args = parser.parse_args()

    iso = Path(args.iso).resolve()
    if not iso.exists():
        print(f"missing ISO: {iso}", file=sys.stderr)
        return 2

    tests = Path("build/tests")
    tests.mkdir(parents=True, exist_ok=True)
    root_image = (tests / "mbr-root.img").resolve()
    disk_image = (tests / "mbr-disk.img").resolve()
    shutil.copyfile("build/disk-fat.img", root_image)
    shutil.copyfile("build/tests/mbr-base.img", disk_image)

    log = (tests / "mbr-partition.log").resolve()
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
        "-drive", f"if=none,id=mbrdisk,file={disk_image},format=raw",
        "-device", "ide-hd,drive=mbrdisk,bus=ide.1",
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

        # Both entries, at the LBAs sfdisk wrote, so a scanner that found the
        # table but misread the offsets still fails here.
        expect("partition: ahci1p1 at LBA 2048, 32768 sectors",
               "first partition not published")
        expect("partition: ahci1p2 at LBA 36864, 32768 sectors",
               "second partition not published")

        # ahci1 itself must never win the root search: it carries a table.
        expect("rootfs: fat mounted from ahci0 at /", "root did not mount")
        expect("winman: ready", "desktop did not come up")

        qmp = Qmp(port, deadline)
        # Mounting p1 is what proves the slice arithmetic: its superblock is
        # 2048 sectors into the disk, so an offset that is wrong by any amount
        # reads something that is not an ext2 superblock and the mount fails.
        send_text(qmp, "mount ahci1p1 /mnt ext2\n")
        expect("mount: ahci1p1 (ext2) at /mnt", "ext2 partition did not mount")

        # Offering the whole partitioned disk must fail, and must fail with
        # EINVAL -- "every backend read it and refused" -- rather than with
        # ENOENT, which would mean the disk had not been published at all.
        send_text(qmp, "mount ahci1 /mnt9\n")
        expect("no supported filesystem found on ahci1",
               "partitioned disk did not report EINVAL")

        # p2 is FAT and deliberately not mounted here. The FAT engine keeps one
        # global volume (mounted_super in kernel/fs/fat/fat_vfs.c), so with a
        # FAT root already mounted a second FAT volume is refused whatever
        # device it sits on. That limit is upstream of partitions; asserting it
        # here would only pin down behaviour this test does not own.
        send_text(qmp, "mount ahci1p1 /mnt3 ext2\n")
        expect("mount: ahci1p1 (ext2) at /mnt3",
               "partition could not be mounted twice")

        text = log.read_text(errors="replace")
        if "PANIC" in text:
            raise RuntimeError(f"kernel panic\n{text}")
        # The whole disk is published for formatting, but offering it to a
        # filesystem must never succeed -- that would mean something matched
        # the partition table itself.
        if "mounted from ahci1 at" in text:
            raise RuntimeError(f"partitioned disk mounted as a volume\n{text}")
    finally:
        if qmp:
            qmp.close()
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)

    print(f"mbr: ahci1 table read, both slices published, p1 mounted; {log}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

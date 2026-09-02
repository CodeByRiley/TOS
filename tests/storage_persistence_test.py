#!/usr/bin/env python3
"""Boot twice with a disposable AHCI/USB ext2 disk; prove writes survive QEMU.

Requires a built ISO, build/disk-ext2.img (AHCI root), and ext2-base.img (USB).
Only copies under build/tests are modified; never pass a physical host disk.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import time

from kernel_panic_test import Qmp, available_port, wait_for_text
from path_lookup_test import send_text


def boot(args, image, pass_number):
    log = Path(f"build/tests/{args.transport}-boot-{pass_number}.log").resolve()
    log.write_bytes(b"")
    port = available_port()
    command = [args.qemu, "-cdrom", str(Path(args.iso).resolve()),
               "-serial", f"file:{log}", "-display", "none", "-vga", "virtio",
               "-qmp", f"tcp:127.0.0.1:{port},server=on,wait=off",
               "-no-reboot", "-m", "512M", "-smp", "2",
               "-drive", f"if=none,id=testdisk,file={image},format=raw"]
    if args.transport == "ahci":
        command += ["-machine", "q35", "-device", "ide-hd,drive=testdisk,bus=ide.0"]
        mounted = "rootfs: ext2 mounted from AHCI SATA drive"
        path = "/persist.txt"
    else:
        command += ["-device", "ich9-usb-ehci1,id=ehci", "-device",
                    "usb-storage,drive=testdisk,bus=ehci.0"]
        mounted = "/usb0"
        path = "/usb0/persist.txt"
    proc = subprocess.Popen(command, stdin=subprocess.DEVNULL)
    qmp = None
    try:
        deadline = time.monotonic() + args.timeout
        for expected in (mounted, "winman: ready"):
            if not wait_for_text(log, expected, deadline):
                raise RuntimeError(f"missing {expected}\n{log.read_text(errors='replace')}")
        qmp = Qmp(port, deadline)
        if pass_number == 1:
            send_text(qmp, f"write {path} {args.transport}-persisted\n")
            # A separate command ensures the synchronous write has returned.
            send_text(qmp, "hello\n")
            if not wait_for_text(log, "hello from ring 3", deadline):
                raise RuntimeError("shell did not return after disk write")
        else:
            send_text(qmp, f"cat {path}\n")
            if not wait_for_text(log, f"{args.transport}-persisted", deadline):
                raise RuntimeError(f"data did not survive reboot\n{log.read_text(errors='replace')}")
        if "PANIC" in log.read_text(errors="replace"):
            raise RuntimeError("kernel panic detected")
    finally:
        if qmp:
            qmp.close()
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--transport", choices=("ahci", "usb"), required=True)
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--iso", default="dist/x86_64/kernel.iso")
    parser.add_argument("--timeout", type=float, default=180)
    args = parser.parse_args()
    source = "build/disk-ext2.img" if args.transport == "ahci" else "build/tests/ext2-base.img"
    image = Path(f"build/tests/{args.transport}-persistence.img").resolve()
    shutil.copyfile(source, image)
    boot(args, image, 1)
    boot(args, image, 2)
    print(f"{args.transport}: ext2 write survived a complete QEMU restart; disk: {image}")


if __name__ == "__main__":
    main()

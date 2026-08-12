#!/usr/bin/env python3
"""Verify exiting a direct framebuffer client cannot free scanout pages."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import time
from pathlib import Path

from kernel_panic_test import Qmp, available_port
from path_lookup_test import send_text
from virtio_resize_test import (
    available_vnc_display,
    request_vnc_resize,
    wait_for_text,
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--iso", default="dist/x86_64/kernel.iso")
    parser.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args()

    qemu = shutil.which(args.qemu) or args.qemu
    iso = Path(args.iso).resolve()
    if not iso.exists():
        print(f"missing ISO: {iso}", file=sys.stderr)
        return 2

    display, vnc_port = available_vnc_display()
    qmp_port = available_port()
    log_path = Path("build/qemu-fb-mapping-lifetime.log").resolve()
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_bytes(b"")

    command = [
        qemu,
        "-cdrom", str(iso),
        "-serial", f"file:{log_path}",
        "-display", "none",
        "-vnc", f"127.0.0.1:{display}",
        "-vga", "virtio",
        "-qmp", f"tcp:127.0.0.1:{qmp_port},server=on,wait=off",
        "-no-reboot",
        "-m", "256M",
        "-smp", "2",
    ]

    proc = subprocess.Popen(command, stdin=subprocess.DEVNULL)
    qmp: Qmp | None = None
    try:
        deadline = time.monotonic() + args.timeout
        if not wait_for_text(log_path, "winman: ready", deadline):
            print(log_path.read_text(encoding="utf-8", errors="replace"))
            print("winman did not become ready", file=sys.stderr)
            return 1

        qmp = Qmp(qmp_port, deadline)
        send_text(qmp, "gfx\n")
        if not wait_for_text(log_path, "start ticks", deadline):
            print(log_path.read_text(encoding="utf-8", errors="replace"))
            print("direct framebuffer client did not start", file=sys.stderr)
            return 1

        qmp.command("human-monitor-command", {"command-line": "sendkey esc"})
        if not wait_for_text(log_path, "We've hit ESC", deadline):
            print(log_path.read_text(encoding="utf-8", errors="replace"))
            print("direct framebuffer client did not exit", file=sys.stderr)
            return 1

        # Let the foreground exec return and its parent reap the client PML4.
        # With an owning framebuffer PTE, this freed the live scanout pool.
        time.sleep(0.5)
        send_text(qmp, "deskelf\n")
        if not wait_for_text(log_path, "deskelf: ready handle=", deadline):
            print(log_path.read_text(encoding="utf-8", errors="replace"))
            print("post-framebuffer process did not become ready",
                  file=sys.stderr)
            return 1

        for width, height in ((1446, 1082), (801, 669), (1280, 800)):
            request_vnc_resize(vnc_port, width, height)
            marker = f"winman: rebound fb to {width}x{height}"
            if not wait_for_text(log_path, marker, deadline):
                print(log_path.read_text(encoding="utf-8", errors="replace"))
                print(f"resize to {width}x{height} did not complete",
                      file=sys.stderr)
                return 1

        time.sleep(0.5)
        log = log_path.read_text(encoding="utf-8", errors="replace")
        if "KERNEL PANIC" in log or "DOUBLE PANIC" in log:
            print(log)
            print("framebuffer lifetime corruption reproduced", file=sys.stderr)
            return 1

        send_text(qmp, "q")
        if not wait_for_text(log_path, "deskelf: exit", deadline):
            print(log_path.read_text(encoding="utf-8", errors="replace"))
            print("guest stopped responding after resize", file=sys.stderr)
            return 1

        print("framebuffer mapping survived client exit and repeated resize")
        return 0
    finally:
        if qmp is not None:
            qmp.close()
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)


if __name__ == "__main__":
    raise SystemExit(main())

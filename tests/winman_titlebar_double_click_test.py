#!/usr/bin/env python3
"""Verify that double-clicking a Winman titlebar maximizes and restores it."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import time
from pathlib import Path

from kernel_panic_test import Qmp, available_port, read_ppm, wait_for_text


FOCUSED_TITLEBAR = bytes((0x00, 0x00, 0xA0))


def focused_titlebar_pixels(path: Path, y: int) -> int:
    width, height, pixels = read_ppm(path)
    if y < 0 or y >= height:
        return 0
    row = pixels[y * width * 3:(y + 1) * width * 3]
    return sum(
        row[offset:offset + 3] == FOCUSED_TITLEBAR
        for offset in range(0, len(row), 3)
    )


def mouse_button(qmp: Qmp, pressed: bool) -> None:
    qmp.command(
        "human-monitor-command",
        {"command-line": f"mouse_button {1 if pressed else 0}"},
    )


def click(qmp: Qmp) -> None:
    mouse_button(qmp, True)
    time.sleep(0.04)
    mouse_button(qmp, False)
    time.sleep(0.08)


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

    build = Path("build").resolve()
    build.mkdir(parents=True, exist_ok=True)
    log_path = build / "qemu-winman-titlebar-double-click.log"
    before = build / "winman-titlebar-before.ppm"
    maximized = build / "winman-titlebar-maximized.ppm"
    restored = build / "winman-titlebar-restored.ppm"
    log_path.write_bytes(b"")
    for screenshot in (before, maximized, restored):
        screenshot.unlink(missing_ok=True)

    qmp_port = available_port()
    command = [
        qemu,
        "-cdrom", str(iso),
        "-serial", f"file:{log_path}",
        "-display", "none",
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
        time.sleep(0.2)
        qmp.command("screendump", {"filename": before.as_posix()})

        # The initial console starts at (16,16). Move from the boot-time
        # pointer origin into the button-free portion of its titlebar.
        qmp.command(
            "human-monitor-command",
            {"command-line": "mouse_move 200 24"},
        )
        click(qmp)
        click(qmp)
        time.sleep(0.3)
        qmp.command("screendump", {"filename": maximized.as_posix()})

        before_top = focused_titlebar_pixels(before, 8)
        maximized_top = focused_titlebar_pixels(maximized, 8)
        if maximized_top < before_top + 300:
            print(
                "double-click did not move the titlebar to the top edge: "
                f"before={before_top}, maximized={maximized_top}",
                file=sys.stderr,
            )
            return 1

        # Maximizing moves the titlebar from y=16 to y=0; reposition the
        # pointer into it and verify that another double-click restores.
        qmp.command(
            "human-monitor-command",
            {"command-line": "mouse_move 0 -16"},
        )
        click(qmp)
        click(qmp)
        time.sleep(0.3)
        qmp.command("screendump", {"filename": restored.as_posix()})

        restored_top = focused_titlebar_pixels(restored, 8)
        if restored_top > before_top + 64:
            print(
                "second double-click did not restore the window: "
                f"before={before_top}, restored={restored_top}",
                file=sys.stderr,
            )
            return 1

        log = log_path.read_text(encoding="utf-8", errors="replace")
        if "PANIC" in log:
            print(log)
            print("kernel panic detected", file=sys.stderr)
            return 1

        print("Winman titlebar double-click maximized and restored the console")
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

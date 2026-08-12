#!/usr/bin/env python3
"""Boot TOS and smoke-test Deskelf's libwm event loop."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import time
from pathlib import Path

from kernel_panic_test import Qmp, available_port, read_ppm, wait_for_text
from path_lookup_test import send_text


def deskelf_pixels_present(path: Path) -> bool:
    width, height, pixels = read_ppm(path)
    if width < 560 or height < 380:
        return False

    face = bytes((0xC0, 0xC0, 0xC0))
    face_pixels = sum(
        pixels[offset:offset + 3] == face
        for offset in range(0, len(pixels), 3)
    )
    return face_pixels > 40_000


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

    log_path = Path("build/qemu-deskelf.log").resolve()
    screenshot = Path("build/deskelf.ppm").resolve()
    moved_screenshot = Path("build/deskelf-selection-moved.ppm").resolve()
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_bytes(b"")
    screenshot.unlink(missing_ok=True)
    moved_screenshot.unlink(missing_ok=True)
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
        send_text(qmp, "deskelf\n")
        if not wait_for_text(log_path, "deskelf: ready handle=", deadline):
            print(log_path.read_text(encoding="utf-8", errors="replace"))
            print("Deskelf did not enter its window event loop", file=sys.stderr)
            return 1

        time.sleep(0.2)
        qmp.command("screendump", {"filename": screenshot.as_posix()})
        if not screenshot.exists() or not deskelf_pixels_present(screenshot):
            print("Deskelf's rendered libui surface was not detected",
                  file=sys.stderr)
            return 1

        # KEY_J is not ASCII 'j'. A raw-code cast would leave the selection
        # unchanged, while keymap_to_ascii moves the accent marker down.
        send_text(qmp, "j")
        time.sleep(0.2)
        qmp.command("screendump", {"filename": moved_screenshot.as_posix()})
        if screenshot.read_bytes() == moved_screenshot.read_bytes():
            print("Deskelf selection did not react to translated input",
                  file=sys.stderr)
            return 1

        # Winman must send WM_EV_QUIT and let the owner destroy the shared
        # surface instead of unmapping it while the process is still drawing.
        qmp.command("human-monitor-command",
                    {"command-line": "mouse_move 612 67"})
        time.sleep(0.2)
        qmp.command("human-monitor-command",
                    {"command-line": "mouse_button 1"})
        time.sleep(0.1)
        qmp.command("human-monitor-command",
                    {"command-line": "mouse_button 0"})
        if not wait_for_text(log_path, "deskelf: exit", deadline):
            print(log_path.read_text(encoding="utf-8", errors="replace"))
            print("Winman close did not reach Deskelf", file=sys.stderr)
            return 1
        if not wait_for_text(log_path, "winman: destroy handle=", deadline):
            print(log_path.read_text(encoding="utf-8", errors="replace"))
            print("Winman did not finish Deskelf teardown", file=sys.stderr)
            return 1

        log = log_path.read_text(encoding="utf-8", errors="replace")
        if ("winman: close button -> request handle=" not in log or
                "PANIC" in log or
                "deskelf: could not create window" in log):
            print(log)
            print("Deskelf smoke test detected a guest failure", file=sys.stderr)
            return 1

        print("Deskelf rendered, handled input, and honored Winman's close")
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

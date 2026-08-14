#!/usr/bin/env python3
"""Catch Winman writes outside a partial-composition clip.

Typing dirties individual console cells. If draw_chrome or another compositor
primitive ignores that clip, it overwrites the persistent backbuffer with the
gray chrome colour. Later cursor repairs copy those stale gray pixels to the
scanout, producing a trail of rectangular blocks.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import time
from pathlib import Path

from kernel_panic_test import Qmp, available_port, read_ppm, wait_for_text
from path_lookup_test import send_text


CHROME_GRAY = bytes((0xC0, 0xC0, 0xC0))


def console_gray_pixels(path: Path) -> int:
    width, height, pixels = read_ppm(path)

    # con_alloc() places the console at (16,16), with a one-pixel border and
    # a 16-pixel titlebar. Stay inside the client area and away from its
    # rounded-down right/bottom edges.
    x0, y0 = 17, 32
    x1, y1 = max(x0, width - 17), max(y0, height - 48)
    count = 0
    for y in range(y0, y1):
        row = y * width * 3
        for x in range(x0, x1):
            offset = row + x * 3
            if pixels[offset:offset + 3] == CHROME_GRAY:
                count += 1
    return count


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

    log_path = Path("build/qemu-winman-partial-repaint.log").resolve()
    before = Path("build/winman-partial-before.ppm").resolve()
    after = Path("build/winman-partial-after.ppm").resolve()
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_bytes(b"")
    before.unlink(missing_ok=True)
    after.unlink(missing_ok=True)
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
        time.sleep(0.3)
        qmp.command("screendump", {"filename": before.as_posix()})
        baseline = console_gray_pixels(before)

        # Generate many cell-sized partial redraws, then move the cursor over
        # the console so its repair path samples the persistent backbuffer.
        send_text(qmp, "gggggggggggggggggggggggggggggggg\n")
        for dx, dy in ((120, 120), (80, 0), (0, 70), (-70, 0),
                       (0, 70), (90, 0), (0, 70), (-80, 0)):
            qmp.command(
                "human-monitor-command",
                {"command-line": f"mouse_move {dx} {dy}"},
            )
            time.sleep(0.08)

        time.sleep(0.3)
        qmp.command("screendump", {"filename": after.as_posix()})
        damaged = console_gray_pixels(after)

        # Antialiased TTF glyphs can contain a handful of pixels whose value
        # happens to equal the chrome colour. A broken compositor adds solid
        # cursor-sized blocks, so allow a small amount of normal text growth.
        if damaged > baseline + 256:
            print(
                f"gray chrome leaked into console client: "
                f"before={baseline}, after={damaged}",
                file=sys.stderr,
            )
            return 1

        log = log_path.read_text(encoding="utf-8", errors="replace")
        if "PANIC" in log:
            print(log)
            print("kernel panic detected", file=sys.stderr)
            return 1

        print(
            "Winman partial repaint preserved the backbuffer "
            f"(gray pixels {baseline} -> {damaged})"
        )
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

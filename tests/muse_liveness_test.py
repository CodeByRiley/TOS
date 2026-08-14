#!/usr/bin/env python3
"""Verify Winman continues presenting while Muse streams audio."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import time
from pathlib import Path

from kernel_panic_test import Qmp, available_port, read_ppm
from path_lookup_test import send_text
from system_stress_test import wait_for_marker


def dump(qmp: Qmp, path: Path) -> tuple[int, int, bytes]:
    path.unlink(missing_ok=True)
    qmp.command("screendump", {"filename": path.as_posix()})
    return read_ppm(path)


def changed_pixels(before: bytes, after: bytes) -> int:
    return sum(before[i:i + 3] != after[i:i + 3]
               for i in range(0, len(before), 3))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--iso", default="dist/x86_64/kernel.iso")
    parser.add_argument("--timeout", type=float, default=90)
    parser.add_argument("--audio-driver", default="none")
    parser.add_argument("--hold-seconds", type=float, default=17)
    args = parser.parse_args()

    qemu = shutil.which(args.qemu) or args.qemu
    iso = Path(args.iso).resolve()
    if not iso.exists():
        print(f"missing ISO: {iso}", file=sys.stderr)
        return 2

    log_path = Path("build/qemu-muse-liveness.log").resolve()
    first_path = Path("build/muse-first.ppm").resolve()
    moved_path = Path("build/muse-moved.ppm").resolve()
    late_path = Path("build/muse-late.ppm").resolve()
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_bytes(b"")
    qmp_port = available_port()
    command = [
        qemu,
        "-cdrom", str(iso),
        "-serial", f"file:{log_path}",
        "-display", "none",
        "-vga", "virtio",
        "-qmp", f"tcp:127.0.0.1:{qmp_port},server=on,wait=off",
        "-audiodev", f"{args.audio_driver},id=snd0",
        "-device", "sb16,audiodev=snd0",
        "-no-reboot",
        "-m", "256M",
        "-smp", "4",
    ]

    proc = subprocess.Popen(command, stdin=subprocess.DEVNULL)
    qmp: Qmp | None = None
    try:
        deadline = time.monotonic() + args.timeout
        if not wait_for_marker(log_path, "winman: ready", proc, deadline):
            print(log_path.read_text(encoding="utf-8", errors="replace"))
            print("winman did not become ready", file=sys.stderr)
            return 1

        qmp = Qmp(qmp_port, deadline)
        send_text(qmp, "muse /music/beethoven")
        qmp.command("human-monitor-command", {"command-line": "sendkey dot"})
        time.sleep(0.12)
        send_text(qmp, "wav\n")
        if not wait_for_marker(log_path, "muse: playing", proc, deadline):
            print(log_path.read_text(encoding="utf-8", errors="replace"))
            print("Muse did not start playback", file=sys.stderr)
            return 1

        qmp.command("human-monitor-command",
                    {"command-line": "mouse_move 120 80"})
        time.sleep(0.4)
        fw, fh, first = dump(qmp, first_path)
        qmp.command("human-monitor-command",
                    {"command-line": "mouse_move -80 40"})
        time.sleep(0.4)
        mw, mh, moved = dump(qmp, moved_path)
        if (fw, fh) != (mw, mh) or changed_pixels(first, moved) == 0:
            print("Winman did not repaint after cursor movement", file=sys.stderr)
            return 1

        time.sleep(args.hold_seconds)
        qmp.command("human-monitor-command",
                    {"command-line": "mouse_move 80 -40"})
        time.sleep(0.4)
        lw, lh, late = dump(qmp, late_path)
        late_changes = changed_pixels(moved, late)
        if (mw, mh) != (lw, lh) or late_changes == 0:
            print("desktop stopped repainting during playback", file=sys.stderr)
            return 1

        log = log_path.read_text(encoding="utf-8", errors="replace")
        if "PANIC" in log or "KERNEL PANIC" in log:
            print(log)
            print("kernel panic detected", file=sys.stderr)
            return 1

        print(f"Muse playback remained live for {args.hold_seconds:.0f}s "
              f"({late_changes} changed pixels)")
        return 0
    finally:
        if qmp is not None:
            qmp.close()
        if proc.poll() is None:
            proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)


if __name__ == "__main__":
    raise SystemExit(main())

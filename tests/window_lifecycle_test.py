#!/usr/bin/env python3
"""Run 64 in-process Winman create/destroy cycles under QEMU."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import time
from pathlib import Path

from kernel_panic_test import Qmp, available_port
from path_lookup_test import send_text
from system_stress_test import wait_for_marker


def wait_for_count(path: Path, marker: str, count: int,
                   proc: subprocess.Popen, deadline: float) -> bool:
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            return False
        try:
            if path.read_text(encoding="utf-8", errors="replace").count(
                    marker) >= count:
                return True
        except OSError:
            pass
        time.sleep(0.02)
    return False


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--iso", default="dist/x86_64/kernel.iso")
    parser.add_argument("--timeout", type=float, default=90)
    args = parser.parse_args()

    qemu = shutil.which(args.qemu) or args.qemu
    iso = Path(args.iso).resolve()
    if not iso.exists():
        print(f"missing ISO: {iso}", file=sys.stderr)
        return 2

    log_path = Path("build/qemu-window-lifecycle.log").resolve()
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
        "-no-reboot",
        "-m", "256M",
        "-smp", "2",
    ]

    proc = subprocess.Popen(command, stdin=subprocess.DEVNULL)
    qmp: Qmp | None = None
    try:
        deadline = time.monotonic() + args.timeout
        if not wait_for_marker(log_path, "winman: ready", proc, deadline):
            print("winman did not become ready", file=sys.stderr)
            return 1
        qmp = Qmp(qmp_port, deadline)
        send_text(qmp, "stress windows\n")
        if not wait_for_marker(log_path, "stress: PASS", proc, deadline):
            print(log_path.read_text(encoding="utf-8", errors="replace"))
            print("window lifecycle stress did not pass", file=sys.stderr)
            return 1
        if not wait_for_count(log_path, "winman: destroy handle=", 64,
                              proc, deadline):
            print(log_path.read_text(encoding="utf-8", errors="replace"))
            print("Winman did not consume all destroy requests",
                  file=sys.stderr)
            return 1

        log = log_path.read_text(encoding="utf-8", errors="replace")
        if ("stress: windows PASS rounds=64" not in log or
                log.count("winman: create handle=") < 64 or
                log.count("winman: destroy handle=") < 64 or
                "PANIC" in log or "stress: FAIL" in log):
            print(log)
            print("window lifecycle markers were incomplete", file=sys.stderr)
            return 1

        print("completed 64 Winman surface create/destroy cycles")
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

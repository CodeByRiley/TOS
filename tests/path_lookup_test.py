#!/usr/bin/env python3
"""Boot TOS and verify bare commands resolve through the default PATH."""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

from kernel_panic_test import Qmp, available_port, wait_for_text


QEMU_KEYS = {
    " ": "spc",
    "/": "slash",
}


def send_text(qmp: Qmp, value: str) -> None:
    for char in value:
        key = "ret" if char == "\n" else QEMU_KEYS.get(char, char)
        qmp.command(
            "human-monitor-command",
            {"command-line": f"sendkey {key}"},
        )
        time.sleep(0.12)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--iso", default="dist/x86_64/kernel.iso")
    parser.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args()

    iso = Path(args.iso).resolve()
    if not iso.exists():
        print(f"missing ISO: {iso}", file=sys.stderr)
        return 2

    log_path = Path("build/qemu-path-lookup.log").resolve()
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_bytes(b"")
    qmp_port = available_port()
    command = [
        args.qemu,
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
        send_text(qmp, "hello\n")
        if not wait_for_text(log_path, "hello from ring 3", deadline):
            print(log_path.read_text(encoding="utf-8", errors="replace"))
            print("PATH did not resolve hello from /usr/bin", file=sys.stderr)
            return 1

        send_text(qmp, "ls /bin\n")
        if not wait_for_text(log_path, "shutdown.elf", deadline):
            print(log_path.read_text(encoding="utf-8", errors="replace"))
            print("PATH did not resolve ls from /bin", file=sys.stderr)
            return 1

        log = log_path.read_text(encoding="utf-8", errors="replace")
        if "PANIC" in log:
            print(log)
            print("kernel panic detected", file=sys.stderr)
            return 1

        print("default PATH resolved /bin and /usr/bin commands")
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

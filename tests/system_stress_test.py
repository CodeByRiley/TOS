#!/usr/bin/env python3
"""Run the heavy in-guest VM, FAT, IPC, Winman, and process stress test."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import time
from pathlib import Path

from kernel_panic_test import Qmp, available_port
from path_lookup_test import send_text


def wait_for_marker(path: Path, marker: str, proc: subprocess.Popen,
                    deadline: float) -> bool:
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            return False
        try:
            if marker in path.read_text(encoding="utf-8", errors="replace"):
                return True
        except OSError:
            pass
        time.sleep(0.02)
    return False


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--iso", default="dist/x86_64/kernel.iso")
    parser.add_argument("--timeout", type=float, default=240)
    parser.add_argument("--cpus", type=int, default=4)
    parser.add_argument("--memory", default="512M")
    args = parser.parse_args()

    if args.cpus < 2:
        parser.error("--cpus must be at least 2")

    qemu = shutil.which(args.qemu) or args.qemu
    iso = Path(args.iso).resolve()
    if not iso.exists():
        print(f"missing ISO: {iso}", file=sys.stderr)
        return 2

    log_path = Path("build/qemu-system-stress.log").resolve()
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
        "-m", args.memory,
        "-smp", str(args.cpus),
    ]

    proc = subprocess.Popen(command, stdin=subprocess.DEVNULL)
    qmp: Qmp | None = None
    started = time.monotonic()
    try:
        deadline = started + args.timeout
        if not wait_for_marker(log_path, "winman: ready", proc, deadline):
            print(log_path.read_text(encoding="utf-8", errors="replace"))
            print("winman did not become ready", file=sys.stderr)
            return 1

        qmp = Qmp(qmp_port, deadline)
        send_text(qmp, "stress\n")
        if not wait_for_marker(log_path, "stress: PASS", proc, deadline):
            print(log_path.read_text(encoding="utf-8", errors="replace"))
            print("guest stress test did not pass", file=sys.stderr)
            return 1

        log = log_path.read_text(encoding="utf-8", errors="replace")
        required = [
            "stress: memory PASS rounds=96",
            "stress: filesystem PASS rounds=64",
            "stress: shmem PASS rounds=128",
            "stress: windows PASS rounds=64",
            "stress: sync ELF PASS rounds=32",
            "stress: sync PE PASS rounds=8",
            "stress: async PASS rounds=64",
            "stress_peer: clean exit",
        ]
        missing = [marker for marker in required if marker not in log]
        if missing:
            print(log)
            print(f"missing stress stages: {', '.join(missing)}",
                  file=sys.stderr)
            return 1

        forbidden = [
            "stress: FAIL ",
            "stress: FAILURES=",
            "KERNEL PANIC",
            "DOUBLE PANIC",
        ]
        found = [marker for marker in forbidden if marker in log]
        if found or log.count("[BOOT(32)] entered") != 1:
            print(log)
            print(f"guest failure markers: {', '.join(found)}",
                  file=sys.stderr)
            return 1

        ready_count = log.count("process_spawn: ready pid =")
        if ready_count < 66:
            print(log)
            print(f"only {ready_count} deferred processes became ready",
                  file=sys.stderr)
            return 1

        elapsed = time.monotonic() - started
        print(f"system stress passed in {elapsed:.1f}s "
              f"({ready_count} deferred activations)")
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

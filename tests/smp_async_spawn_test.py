#!/usr/bin/env python3
"""Headless QEMU smoke test for AP work and deferred process loading."""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path


def wait_for_text(path: Path, needle: str, deadline: float) -> bool:
    while time.monotonic() < deadline:
        if path.exists():
            try:
                if needle in path.read_text(encoding="utf-8", errors="replace"):
                    return True
            except OSError:
                pass
        time.sleep(0.02)
    return False


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--iso", default="dist/x86_64/kernel.iso")
    parser.add_argument("--cpus", type=int, default=4)
    parser.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args()

    if args.cpus < 2:
        parser.error("--cpus must be at least 2")

    qemu = shutil.which(args.qemu) or args.qemu
    iso = Path(args.iso).resolve()
    if not iso.exists():
        print(f"missing ISO: {iso}", file=sys.stderr)
        return 2

    log_path = Path("build/qemu-smp-async-spawn.log").resolve()
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_bytes(b"")

    command = [
        qemu,
        "-cdrom",
        str(iso),
        "-serial",
        f"file:{log_path}",
        "-display",
        "none",
        "-vga",
        "virtio",
        "-no-reboot",
        "-m",
        "256M",
        "-smp",
        str(args.cpus),
    ]

    proc = subprocess.Popen(command, stdin=subprocess.DEVNULL)
    try:
        deadline = time.monotonic() + args.timeout
        if not wait_for_text(log_path, "winman: ready", deadline):
            print(log_path.read_text(encoding="utf-8", errors="replace"))
            print("winman did not become ready", file=sys.stderr)
            return 1

        log = log_path.read_text(encoding="utf-8", errors="replace")
        required = [
            "SMP: AP workers       =",
            "SMP: jobs completed  =",
            "SMP: worker cpu mask =",
            "SMP: bootstrap CR3    =",
            "SMP: AP target CR3    =",
            "process_spawn: queued pid =",
            "winman: spawn returned pid =",
            "process_spawn: ready pid =",
        ]
        missing = [marker for marker in required if marker not in log]
        if missing:
            print(log)
            print(f"missing markers: {', '.join(missing)}", file=sys.stderr)
            return 1

        if "PANIC" in log or "SMP: AP failed" in log:
            print(log)
            print("kernel panic or AP boot failure detected", file=sys.stderr)
            return 1

        smp_patterns = {
            "workers": r"SMP: AP workers\s+=\s+(0x[0-9A-Fa-f]+)",
            "completed": r"SMP: jobs completed\s+=\s+(0x[0-9A-Fa-f]+)",
            "cpu_mask": r"SMP: worker cpu mask\s+=\s+(0x[0-9A-Fa-f]+)",
        }
        smp_matches = {
            name: re.search(pattern, log)
            for name, pattern in smp_patterns.items()
        }
        if any(match is None for match in smp_matches.values()):
            print(log)
            print("could not parse SMP probe results", file=sys.stderr)
            return 1
        smp_values = {
            name: int(match.group(1), 16)
            for name, match in smp_matches.items()
            if match
        }
        if (smp_values["workers"] < 1 or smp_values["completed"] < 1 or
                smp_values["cpu_mask"] == 0 or smp_values["cpu_mask"] & 1):
            print(log)
            print("SMP probe did not execute work on an AP", file=sys.stderr)
            return 1

        cr3_patterns = [
            r"SMP: bootstrap CR3\s+=\s+(0x[0-9A-Fa-f]+)",
            r"SMP: AP target CR3\s+=\s+(0x[0-9A-Fa-f]+)",
        ]
        cr3_matches = [re.search(pattern, log) for pattern in cr3_patterns]
        if any(match is None for match in cr3_matches):
            print(log)
            print("could not parse AP CR3 handoff", file=sys.stderr)
            return 1
        cr3_values = [int(match.group(1), 16) for match in cr3_matches if match]
        if len(cr3_values) != 2 or cr3_values[0] == cr3_values[1]:
            print(log)
            print("AP handoff did not switch to a distinct PML4",
                  file=sys.stderr)
            return 1

        queued = log.index("process_spawn: queued pid =")
        returned = log.index("winman: spawn returned pid =")
        ready = log.index("process_spawn: ready pid =")
        if not queued < returned < ready:
            print(log)
            print("spawn did not return between queueing and image readiness",
                  file=sys.stderr)
            return 1

        pid_patterns = [
            r"process_spawn: queued pid =\s+(0x[0-9A-Fa-f]+)",
            r"winman: spawn returned pid =\s+(0x[0-9A-Fa-f]+)",
            r"process_spawn: ready pid =\s+(0x[0-9A-Fa-f]+)",
        ]
        pids = [re.search(pattern, log) for pattern in pid_patterns]
        pid_values = {match.group(1).lower() for match in pids if match}
        if any(match is None for match in pids) or len(pid_values) != 1:
            print(log)
            print("queued, returned, and ready PIDs do not match",
                  file=sys.stderr)
            return 1

        print(f"SMP work and deferred spawn passed with {args.cpus} CPUs")
        return 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)


if __name__ == "__main__":
    raise SystemExit(main())

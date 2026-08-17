#!/usr/bin/env python3
"""Inject an NMI and verify serial and graphical kernel panic output."""

from __future__ import annotations

import argparse
import json
import re
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path


def available_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


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


class Qmp:
    def __init__(self, port: int, deadline: float) -> None:
        while True:
            try:
                self.sock = socket.create_connection(
                    ("127.0.0.1", port), timeout=2
                )
                break
            except OSError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.05)
        self.stream = self.sock.makefile("rwb", buffering=0)
        self._read_response("QMP")
        self.command("qmp_capabilities")

    def _read_response(self, key: str) -> dict:
        while True:
            line = self.stream.readline()
            if not line:
                raise RuntimeError("QMP connection closed")
            message = json.loads(line)
            if "error" in message:
                raise RuntimeError(f"QMP command failed: {message['error']}")
            if key in message:
                return message

    def command(self, name: str, arguments: dict | None = None) -> dict:
        request: dict[str, object] = {"execute": name}
        if arguments is not None:
            request["arguments"] = arguments
        self.stream.write(json.dumps(request).encode("ascii") + b"\n")
        response = self._read_response("return")
        return response

    def close(self) -> None:
        self.stream.close()
        self.sock.close()


def read_ppm(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    if not data.startswith(b"P6"):
        raise RuntimeError("QEMU screendump is not a binary PPM")

    pos = 2
    tokens: list[bytes] = []
    while len(tokens) < 3:
        while pos < len(data) and data[pos] in b" \t\r\n":
            pos += 1
        if pos < len(data) and data[pos] == ord("#"):
            while pos < len(data) and data[pos] != ord("\n"):
                pos += 1
            continue
        start = pos
        while pos < len(data) and data[pos] not in b" \t\r\n":
            pos += 1
        tokens.append(data[start:pos])
    while pos < len(data) and data[pos] in b" \t\r\n":
        pos += 1

    width, height, maximum = (int(token) for token in tokens)
    if maximum != 255:
        raise RuntimeError(f"unsupported PPM maximum: {maximum}")
    pixels = data[pos:]
    if len(pixels) != width * height * 3:
        raise RuntimeError("truncated QEMU screendump")
    return width, height, pixels


FRAME_RE = re.compile(r"^\s*\d+:\s*([0-9a-fA-F]+)\s*:\s*([0-9a-fA-F]+)\s+(\S+)")
KTEXT_RE = re.compile(r"Kernel text:\s*0x([0-9a-fA-F]+)\s*-\s*0x([0-9a-fA-F]+)")
SYMBOL_RE = re.compile(r"^[A-Za-z_][A-Za-z_0-9.$]*\+0x[0-9a-fA-F]+$")

# panic_symstr() prints "kernel+0xNNN" when symtab_resolve() returns NULL but
# the address is inside the kernel text. That is the shape of a *failed*
# lookup, and it satisfies SYMBOL_RE, so it has to be rejected by name — an
# empty symbol table otherwise produces a backtrace this check calls resolved.
UNRESOLVED_NAMES = {"kernel"}


def backtrace_symbols(log: str) -> tuple[int, list[str]]:
    """Check the backtrace resolved to names rather than bare addresses.

    Returns (resolved_count, complaints). The header alone proves nothing —
    a dropped .ksymtab section, a table the second link pass failed to
    override, or offsets computed against the wrong base all still print a
    well-formed backtrace, just one made entirely of hex. So every return
    address that lands inside the kernel text has to carry a symbol.
    """
    ktext = KTEXT_RE.search(log)
    if not ktext:
        return 0, ["no 'Kernel text:' bounds line to range-check against"]
    lo, hi = int(ktext.group(1), 16), int(ktext.group(2), 16)

    lines = log.splitlines()
    try:
        start = next(i for i, l in enumerate(lines)
                     if "Backtrace (frame : return address):" in l)
    except StopIteration:
        return 0, ["no backtrace section"]

    resolved, complaints = 0, []
    for line in lines[start + 1:]:
        m = FRAME_RE.match(line)
        if not m:
            break  # first non-frame line ends the section
        ret, tail = int(m.group(2), 16), m.group(3)
        named = (SYMBOL_RE.match(tail)
                 and tail.split("+", 1)[0] not in UNRESOLVED_NAMES)
        if named:
            resolved += 1
        elif lo <= ret < hi:
            # In-kernel address that symtab_resolve could not name.
            complaints.append(f"unresolved kernel frame 0x{ret:x} -> {tail!r}")

    if resolved == 0:
        complaints.append("backtrace resolved no symbols at all")
    return resolved, complaints


def panic_pixels_present(path: Path) -> bool:
    width, height, pixels = read_ppm(path)
    if width < 320 or height < 240:
        return False

    background = (0x25, 0x27, 0x2B)
    accent = (0xE0, 0x6C, 0x75)
    bg_count = 0
    accent_count = 0
    for offset in range(0, len(pixels), 3):
        pixel = tuple(pixels[offset:offset + 3])
        if pixel == background:
            bg_count += 1
        elif pixel == accent:
            accent_count += 1
    total = width * height
    return bg_count > total // 2 and accent_count >= width * 3


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

    log_path = Path("build/qemu-kernel-panic.log").resolve()
    screenshot = Path("build/kernel-panic.ppm").resolve()
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_bytes(b"")
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
        "-smp", "1",
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
        qmp.command("inject-nmi")
        if not wait_for_text(log_path, "Please restart the machine.", deadline):
            print(log_path.read_text(encoding="utf-8", errors="replace"))
            print("panic report did not complete", file=sys.stderr)
            return 1

        required = [
            "*** TOS KERNEL PANIC ***",
            "panic(cpu 0 caller ",
            "Exception: vector 2 (NMI)",
            "Panicked task:",
            "Kernel version:",
            "Control registers:",
            "Registers:",
            "Backtrace (frame : return address):",
        ]
        log = log_path.read_text(encoding="utf-8", errors="replace")

        resolved, complaints = backtrace_symbols(log)
        if complaints:
            print(log)
            for complaint in complaints:
                print(f"backtrace: {complaint}", file=sys.stderr)
            print("kernel symbol table did not resolve the backtrace",
                  file=sys.stderr)
            return 1
        print(f"backtrace resolved {resolved} symbol(s)")

        missing = [marker for marker in required if marker not in log]
        if missing:
            print(log)
            print(f"missing panic fields: {', '.join(missing)}", file=sys.stderr)
            return 1

        time.sleep(0.2)
        qmp.command("screendump", {"filename": screenshot.as_posix()})
        if not screenshot.exists() or not panic_pixels_present(screenshot):
            print("panic framebuffer presentation was not detected",
                  file=sys.stderr)
            return 1

        print("kernel panic serial report and restart screen passed")
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

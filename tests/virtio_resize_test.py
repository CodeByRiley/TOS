#!/usr/bin/env python3
"""Headless QEMU smoke test for host-driven virtio-gpu resize latency."""

from __future__ import annotations

import argparse
import shutil
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path


def recv_exact(sock: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise RuntimeError("VNC server closed the connection")
        data.extend(chunk)
    return bytes(data)


def request_vnc_resize(port: int, width: int, height: int) -> None:
    with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
        version = recv_exact(sock, 12)
        if not version.startswith(b"RFB "):
            raise RuntimeError(f"invalid VNC greeting: {version!r}")
        sock.sendall(b"RFB 003.008\n")

        count = recv_exact(sock, 1)[0]
        security_types = recv_exact(sock, count)
        if 1 not in security_types:
            raise RuntimeError("QEMU VNC did not offer no-auth security")
        sock.sendall(b"\x01")
        if struct.unpack(">I", recv_exact(sock, 4))[0] != 0:
            raise RuntimeError("QEMU VNC rejected the connection")

        sock.sendall(b"\x01")  # shared ClientInit
        server_init = recv_exact(sock, 24)
        name_length = struct.unpack(">I", server_init[20:24])[0]
        recv_exact(sock, name_length)

        # Advertise DesktopSize and ExtendedDesktopSize support.
        sock.sendall(struct.pack(">BBHii", 2, 0, 2, -223, -308))

        # RFB SetDesktopSize with one screen covering the requested desktop.
        message = struct.pack(
            ">BBHHBBIHHHHI",
            251,
            0,
            width,
            height,
            1,
            0,
            0,
            0,
            0,
            width,
            height,
            0,
        )
        sock.sendall(message)
        sock.sendall(struct.pack(">BBHHHH", 3, 0, 0, 0, width, height))


def wait_for_text(path: Path, needle: str, deadline: float) -> bool:
    while time.monotonic() < deadline:
        if path.exists():
            try:
                if needle in path.read_text(encoding="utf-8", errors="replace"):
                    return True
            except OSError:
                pass
        time.sleep(0.01)
    return False


def available_vnc_display() -> tuple[int, int]:
    for display in range(20, 100):
        port = 5900 + display
        with socket.socket() as probe:
            try:
                probe.bind(("127.0.0.1", port))
            except OSError:
                continue
        return display, port
    raise RuntimeError("no free local VNC display")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--iso", default="dist/x86_64/kernel.iso")
    parser.add_argument("--width", type=int, default=1600)
    parser.add_argument("--height", type=int, default=900)
    parser.add_argument("--boot-timeout", type=float, default=60)
    parser.add_argument("--resize-timeout", type=float, default=10)
    args = parser.parse_args()

    qemu = shutil.which(args.qemu) or args.qemu
    iso = Path(args.iso).resolve()
    if not iso.exists():
        print(f"missing ISO: {iso}", file=sys.stderr)
        return 2

    display, port = available_vnc_display()
    log_path = Path("build/qemu-virtio-resize.log").resolve()
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
        "-vnc",
        f"127.0.0.1:{display}",
        "-vga",
        "virtio",
        "-no-reboot",
        "-m",
        "256M",
    ]

    proc = subprocess.Popen(command, stdin=subprocess.DEVNULL)
    try:
        boot_deadline = time.monotonic() + args.boot_timeout
        if not wait_for_text(log_path, "winman: ready", boot_deadline):
            print(log_path.read_text(encoding="utf-8", errors="replace"))
            print("winman did not become ready", file=sys.stderr)
            return 1

        started = time.monotonic()
        request_vnc_resize(port, args.width, args.height)
        marker = f"winman: rebound fb to {args.width}x{args.height}"
        if not wait_for_text(log_path, marker, started + args.resize_timeout):
            print(log_path.read_text(encoding="utf-8", errors="replace"))
            print(f"resize marker not seen within {args.resize_timeout:.1f}s",
                  file=sys.stderr)
            return 1

        elapsed_ms = (time.monotonic() - started) * 1000
        print(f"virtio resize {args.width}x{args.height}: {elapsed_ms:.1f} ms")
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

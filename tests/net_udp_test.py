#!/usr/bin/env python3
"""Send a UDP datagram to TOS and check it comes back.

This is the test for the Linux socket syscall numbers. udpecho is built
against musl's <sys/socket.h> and contains nothing TOS-specific, so every
call it makes goes out on the Linux numbers: socket is 41, bind 49,
sendto 44, recvfrom 45. Before those were mirrored in the kernel the
program did not misbehave subtly, it died at the first call and the serial
log filled with "unknown syscall = 0x29".

Testing it from outside, over a real UDP hostfwd, is what makes the result
mean something. A round trip proves the whole chain at once: bind claimed
a port, the inbound datagram was demultiplexed to that socket, recvfrom
returned both payload and a usable source address, and sendto put a reply
back on the wire addressed from that source. Checking any one of those in
isolation would let the others stay broken.

It also stands in for ported software generally. NetSurf's fetcher will
reach the network through these same entry points; a TOS-specific socket
API would have proved nothing about whether they work.
"""

from __future__ import annotations

import argparse
import socket
import subprocess
import sys
import time
from pathlib import Path

from kernel_panic_test import Qmp, available_port, wait_for_text
from path_lookup_test import send_text

GUEST_ECHO_PORT = 7777
PAYLOAD = b"tos-udp-roundtrip"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--iso", default="dist/x86_64/kernel.iso")
    parser.add_argument("--timeout", type=float, default=120)
    args = parser.parse_args()

    iso = Path(args.iso).resolve()
    if not iso.exists():
        print(f"missing ISO: {iso}", file=sys.stderr)
        return 2

    log_path = Path("build/qemu-net-udp.log").resolve()
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_bytes(b"")

    qmp_port = available_port()
    fwd_port = available_port()

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
        "-netdev",
        f"user,id=n0,hostfwd=udp::{fwd_port}-10.0.2.30:{GUEST_ECHO_PORT}",
        "-device", "e1000,netdev=n0",
    ]

    proc = subprocess.Popen(command, stdin=subprocess.DEVNULL)
    qmp: Qmp | None = None
    try:
        deadline = time.monotonic() + args.timeout
        if not wait_for_text(log_path, "winman: ready", deadline):
            print(log_path.read_text(encoding="utf-8", errors="replace"))
            print("winman did not become ready", file=sys.stderr)
            return 1
        if not wait_for_text(log_path, "e1000: RX/TX ready", deadline):
            print("e1000 did not initialise", file=sys.stderr)
            return 1

        qmp = Qmp(qmp_port, deadline)
        send_text(qmp, "udpecho 2\n")

        if not wait_for_text(log_path, "udpecho: listening", deadline):
            log = log_path.read_text(encoding="utf-8", errors="replace")
            print(log[-3000:])
            if "unknown syscall" in log:
                print("kernel reported unknown syscall -- the Linux socket "
                      "numbers (41/44/45/49) are not wired up", file=sys.stderr)
            else:
                print("udpecho did not bind", file=sys.stderr)
            return 1

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(5.0)
        echoed = None
        try:
            for attempt in range(4):
                sock.sendto(PAYLOAD, ("127.0.0.1", fwd_port))
                try:
                    data, _ = sock.recvfrom(2048)
                except socket.timeout:
                    continue
                echoed = data
                break
        finally:
            sock.close()

        log = log_path.read_text(encoding="utf-8", errors="replace")
        if "PANIC" in log:
            print(log[-4000:])
            print("kernel panic detected", file=sys.stderr)
            return 1

        if "unknown syscall" in log:
            for line in log.splitlines():
                if "unknown syscall" in line:
                    print("  " + line.strip())
            print("kernel reported unknown syscall during the exchange",
                  file=sys.stderr)
            return 1

        if echoed is None:
            print(log[-3000:])
            print("no datagram came back -- bind and recvfrom may have "
                  "worked while sendto failed to reach the wire",
                  file=sys.stderr)
            return 1

        if echoed != PAYLOAD:
            print(f"echo mismatch: sent {PAYLOAD!r}, got {echoed!r}",
                  file=sys.stderr)
            return 1

        for line in log.splitlines():
            if line.startswith("udpecho:"):
                print("  " + line.strip())

        print(f"UDP ok: {len(echoed)} bytes echoed intact through musl "
              "socket/bind/recvfrom/sendto")
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

#!/usr/bin/env python3
"""Boot TOS with an e1000, drive traffic at it, and check netmon sees it.

The interesting assertion is that the driver keeps receiving. RDT names the
descriptor the NIC must not write, so releasing a consumed descriptor means
pointing RDT at it rather than one past it; getting that wrong collapses
RDH onto RDT after the very first frame and the NIC silently stops
receiving. That failure looks like a working driver until something sends
a second packet, which is exactly what this test does.

SLIRP will not let the host address the guest directly, but a hostfwd will:
connecting to the forwarded port makes SLIRP ARP for the guest address and
then send TCP SYNs to it. QEMU's filter-dump records what actually crossed
the link, so the guest's own count can be checked against ground truth
rather than against a number chosen here.
"""

from __future__ import annotations

import argparse
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

from kernel_panic_test import Qmp, available_port, wait_for_text
from path_lookup_test import send_text

RECEIVED_MARKER = "[e1000] Received packet!"


def pcap_inbound(path: Path, guest_mac: bytes) -> int:
    """Frames in the dump addressed to the guest, i.e. what it should see.

    filter-dump records both directions, so the guest's own transmissions
    have to come out of the count before it means anything.
    """
    data = path.read_bytes()
    if len(data) < 24:
        return 0
    magic = struct.unpack("<I", data[:4])[0]
    endian = "<" if magic in (0xA1B2C3D4, 0xA1B2CD34) else ">"

    at, inbound = 24, 0
    while at + 16 <= len(data):
        _ts, _us, caplen, _wire = struct.unpack(endian + "IIII", data[at:at + 16])
        at += 16
        frame = data[at:at + caplen]
        at += caplen
        if len(frame) >= 14 and frame[6:12] != guest_mac:
            inbound += 1
    return inbound


def poke(port: int, attempts: int = 4) -> None:
    """Nudge SLIRP into addressing the guest. The connects never complete --
    nothing in TOS answers a SYN yet -- which is the point: the frames
    arrive regardless."""
    for _ in range(attempts):
        sock = socket.socket()
        sock.settimeout(1.0)
        try:
            sock.connect(("127.0.0.1", port))
        except OSError:
            pass
        finally:
            sock.close()
        time.sleep(0.6)


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

    log_path = Path("build/qemu-netmon.log").resolve()
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_bytes(b"")
    pcap_path = Path("build/qemu-netmon.pcap").resolve()
    if pcap_path.exists():
        pcap_path.unlink()

    qmp_port = available_port()
    fwd_port = available_port()

    # 10.0.2.30 is the address the e1000 driver answers for.
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
        "-netdev", f"user,id=n0,hostfwd=tcp::{fwd_port}-10.0.2.30:80",
        "-device", "e1000,netdev=n0",
        "-object", f"filter-dump,id=dump0,netdev=n0,file={pcap_path}",
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
            print(log_path.read_text(encoding="utf-8", errors="replace"))
            print("e1000 did not initialise", file=sys.stderr)
            return 1

        qmp = Qmp(qmp_port, deadline)

        # netmon draws into a window, so a failure to start shows up as a
        # missing process rather than missing output. Launch it first so
        # the capture ring is being drained while traffic arrives.
        send_text(qmp, "netmon\n")
        time.sleep(3.0)

        poke(fwd_port)
        time.sleep(2.5)

        log = log_path.read_text(encoding="utf-8", errors="replace")
        if "PANIC" in log:
            print(log[-4000:])
            print("kernel panic detected", file=sys.stderr)
            return 1

        received = log.count(RECEIVED_MARKER)
        guest_mac = bytes([0x52, 0x54, 0x00, 0x12, 0x34, 0x56])
        expected = pcap_inbound(pcap_path, guest_mac) if pcap_path.exists() else 0

        if received < 2:
            print(log[-3000:])
            print(f"driver received {received} frame(s); RX ring wedged after "
                  f"the first -- check the RDT update in e1000_poll_rx",
                  file=sys.stderr)
            return 1

        if expected and received < expected:
            print(f"driver received {received} of {expected} inbound frames",
                  file=sys.stderr)
            return 1

        print(f"e1000 received {received} frames "
              f"({expected} inbound on the wire); netmon ran without panic")
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

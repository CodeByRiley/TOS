#!/usr/bin/env python3
"""Boot TOS and check the ARP layer both answers and speaks first.

netmon_test.py covers the receive path and the RX descriptor ring. What it
cannot show is *origination* -- a frame TOS composed on its own initiative
rather than by turning a received one around. Every frame the old
driver-resident ARP/ICMP ever sent was a reply, and it found the
destination MAC by copying it out of the request it was answering. Nothing
in that design could have sent a first packet, and no receive-driven test
would have noticed.

The gratuitous ARP at link-up is that first packet. Asserting on it here
pins down the whole transmit chain that UDP, DHCP, DNS, and TCP all sit on:
netif_tx, eth_output, and the driver's descriptor handling.

Ground truth is QEMU's filter-dump rather than the guest's own opinion of
what it sent.

One thing this deliberately does not assert unconditionally: an ARP reply.
The announcement is effective enough that SLIRP caches the binding from it
and never asks, so on a passing run there is no request to reply to -- the
inbound TCP SYNs arriving without a preceding ARP exchange *are* the proof
the announcement was accepted. The reply path is still checked whenever a
request does show up, and the cache/queue/expiry logic underneath it wants
a host unit test rather than a boot.
"""

from __future__ import annotations

import argparse
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

from kernel_panic_test import available_port, wait_for_text

GUEST_MAC = bytes([0x52, 0x54, 0x00, 0x12, 0x34, 0x56])
GUEST_IP = bytes([10, 0, 2, 30])
GATEWAY_IP = bytes([10, 0, 2, 2])
BROADCAST = b"\xff" * 6

ETH_TYPE_ARP = 0x0806
ARP_OP_REQUEST = 1
ARP_OP_REPLY = 2


def pcap_frames(path: Path) -> list[bytes]:
    """Every frame in a classic-format pcap, both directions."""
    data = path.read_bytes()
    if len(data) < 24:
        return []
    magic = struct.unpack("<I", data[:4])[0]
    endian = "<" if magic in (0xA1B2C3D4, 0xA1B2CD34) else ">"

    frames, at = [], 24
    while at + 16 <= len(data):
        _ts, _us, caplen, _wire = struct.unpack(endian + "IIII", data[at:at + 16])
        at += 16
        frames.append(data[at:at + caplen])
        at += caplen
    return frames


def parse_arp(frame: bytes) -> dict | None:
    """Decode an Ethernet+ARP frame, or None if it is not IPv4-over-Ethernet ARP."""
    if len(frame) < 14 + 28:
        return None
    if struct.unpack(">H", frame[12:14])[0] != ETH_TYPE_ARP:
        return None

    htype, ptype, hlen, plen, oper = struct.unpack(">HHBBH", frame[14:22])
    if htype != 1 or ptype != 0x0800 or hlen != 6 or plen != 4:
        return None

    return {
        "eth_dst": frame[0:6],
        "eth_src": frame[6:12],
        "oper": oper,
        "sha": frame[22:28],
        "spa": frame[28:32],
        "tha": frame[32:38],
        "tpa": frame[38:42],
    }


def describe(arp: dict) -> str:
    name = {ARP_OP_REQUEST: "request", ARP_OP_REPLY: "reply"}.get(arp["oper"], "?")
    spa = ".".join(str(b) for b in arp["spa"])
    tpa = ".".join(str(b) for b in arp["tpa"])
    return f"{name} spa={spa} tpa={tpa} src={arp['eth_src'].hex(':')}"


def poke(port: int, attempts: int = 4) -> None:
    """Make SLIRP address the guest. The connects never complete -- nothing
    answers a SYN yet -- but SLIRP has to ARP before it can try."""
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

    log_path = Path("build/qemu-net-arp.log").resolve()
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_bytes(b"")
    pcap_path = Path("build/qemu-net-arp.pcap").resolve()
    if pcap_path.exists():
        pcap_path.unlink()

    fwd_port = available_port()

    command = [
        args.qemu,
        "-cdrom", str(iso),
        "-serial", f"file:{log_path}",
        "-display", "none",
        "-vga", "virtio",
        "-no-reboot",
        "-m", "256M",
        "-smp", "2",
        "-netdev", f"user,id=n0,hostfwd=tcp::{fwd_port}-10.0.2.30:80",
        "-device", "e1000,netdev=n0",
        "-object", f"filter-dump,id=dump0,netdev=n0,file={pcap_path}",
    ]

    proc = subprocess.Popen(command, stdin=subprocess.DEVNULL)
    try:
        deadline = time.monotonic() + args.timeout
        if not wait_for_text(log_path, "e1000: RX/TX ready", deadline):
            print(log_path.read_text(encoding="utf-8", errors="replace"))
            print("e1000 did not initialise", file=sys.stderr)
            return 1

        poke(fwd_port)
        time.sleep(2.0)

        log = log_path.read_text(encoding="utf-8", errors="replace")
        if "PANIC" in log:
            print(log[-4000:])
            print("kernel panic detected", file=sys.stderr)
            return 1

        if not pcap_path.exists():
            print("no capture produced", file=sys.stderr)
            return 1

        arps = [a for a in (parse_arp(f) for f in pcap_frames(pcap_path)) if a]
        from_guest = [a for a in arps if a["eth_src"] == GUEST_MAC]

        for arp in arps:
            print("  " + describe(arp))

        # Origination: a broadcast request for our own address, which only a
        # frame TOS built itself can be.
        announce = [
            a for a in from_guest
            if a["oper"] == ARP_OP_REQUEST
            and a["spa"] == GUEST_IP
            and a["tpa"] == GUEST_IP
            and a["eth_dst"] == BROADCAST
        ]
        if not announce:
            print("no gratuitous ARP from the guest -- transmit never "
                  "originated a frame; check netif_register/arp_announce "
                  "ordering in e1000_init_hardware", file=sys.stderr)
            return 1

        # Resolution, the other half of speaking first: a request aimed at
        # someone else's address, and the reply coming back into the cache.
        # This is the path every outbound datagram will take.
        asked_gateway = [
            a for a in from_guest
            if a["oper"] == ARP_OP_REQUEST
            and a["spa"] == GUEST_IP
            and a["tpa"] == GATEWAY_IP
        ]
        if not asked_gateway:
            print("guest never ARPed the gateway -- arp_prime_gateway did not "
                  "run, or ipv4_next_hop picked the wrong address",
                  file=sys.stderr)
            return 1

        gateway_replies = [
            a for a in arps
            if a["eth_src"] != GUEST_MAC
            and a["oper"] == ARP_OP_REPLY
            and a["spa"] == GATEWAY_IP
            and a["tpa"] == GUEST_IP
        ]
        if not gateway_replies:
            print("gateway never replied to the guest's ARP request -- the "
                  "request went out malformed, or the source address is wrong",
                  file=sys.stderr)
            return 1

        # The peer accepted the announcement: it addressed IPv4 at us without
        # having to ask first. A failed announcement would show up as an
        # inbound ARP request here instead.
        inbound_ip = [
            f for f in pcap_frames(pcap_path)
            if len(f) >= 34 and f[0:6] == GUEST_MAC
            and struct.unpack(">H", f[12:14])[0] == 0x0800
            and f[30:34] == GUEST_IP
        ]
        if not inbound_ip:
            print("no inbound IPv4 addressed to the guest -- the peer never "
                  "accepted the announcement and never resolved the address",
                  file=sys.stderr)
            return 1

        # Only meaningful when something did ask. See the module docstring.
        asked = [
            a for a in arps
            if a["eth_src"] != GUEST_MAC
            and a["oper"] == ARP_OP_REQUEST
            and a["tpa"] == GUEST_IP
        ]
        replies = [
            a for a in from_guest
            if a["oper"] == ARP_OP_REPLY and a["spa"] == GUEST_IP
        ]
        if asked and not replies:
            print(f"{len(asked)} ARP request(s) for the guest went unanswered "
                  "-- check the address filter in eth_input", file=sys.stderr)
            return 1

        answered = f"{len(replies)} reply/replies to {len(asked)} request(s)"
        print(f"ARP ok: {len(announce)} gratuitous announcement(s), "
              f"{len(asked_gateway)} gateway request(s) answered "
              f"{len(gateway_replies)} time(s), {answered}, "
              f"{len(inbound_ip)} inbound IPv4 frame(s) to the guest")
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

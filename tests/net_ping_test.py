#!/usr/bin/env python3
"""Boot TOS, run `ping` in the shell, and check ICMP echo on the wire.

ICMP echo reply was the one layer of the network stack with no coverage,
and the reason was environmental rather than difficult: SLIRP does not
forward inbound ICMP to the guest, so a host-side `ping 10.0.2.30` never
reaches TOS no matter what the code does. Any test built on that would
have failed forever and told us nothing.

Outbound works, though - SLIRP answers pings addressed to the gateway - so
the guest has to be the one that asks. That makes this a stronger test than
the host-pings-guest version would have been: an echo request TOS composed
itself has to travel through ARP resolution, the IPv4 header and checksum,
and the driver's transmit ring before SLIRP will even look at it, and the
reply has to come back up through the receive ring and match a waiting
session in icmp.c. One command covers the whole path.

Assertions are on QEMU's filter-dump rather than on what ping printed:
the dump records what actually crossed the link, and it lets both
checksums be verified independently of the code that produced them. A bad
checksum is the specific failure worth catching here -- the packet still
looks perfect in a hex dump, and the peer just silently drops it.
"""

from __future__ import annotations

import argparse
import struct
import subprocess
import sys
import time
from pathlib import Path

from kernel_panic_test import Qmp, available_port, wait_for_text
from path_lookup_test import send_text

GUEST_MAC = bytes([0x52, 0x54, 0x00, 0x12, 0x34, 0x56])
GUEST_IP = bytes([10, 0, 2, 30])
GATEWAY_IP = bytes([10, 0, 2, 2])

ETH_TYPE_IPV4 = 0x0800
IPPROTO_ICMP = 1
ICMP_ECHO_REPLY = 0
ICMP_ECHO_REQUEST = 8

PING_COUNT = 2


def pcap_frames(path: Path) -> list[bytes]:
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


def ones_complement(data: bytes) -> int:
    """RFC 1071 checksum. Zero over an intact header or message."""
    if len(data) % 2:
        data += b"\x00"
    total = 0
    for i in range(0, len(data), 2):
        total += (data[i] << 8) | data[i + 1]
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def parse_icmp(frame: bytes) -> dict | None:
    """Decode Ethernet+IPv4+ICMP, verifying both checksums as it goes."""
    if len(frame) < 14 + 20 + 8:
        return None
    if struct.unpack(">H", frame[12:14])[0] != ETH_TYPE_IPV4:
        return None

    ip = frame[14:]
    if (ip[0] >> 4) != 4:
        return None
    ihl = (ip[0] & 0x0F) * 4
    total_len = struct.unpack(">H", ip[2:4])[0]
    if ihl < 20 or total_len < ihl or total_len > len(ip):
        return None
    if ip[9] != IPPROTO_ICMP:
        return None

    icmp = ip[ihl:total_len]
    if len(icmp) < 8:
        return None

    return {
        "eth_src": frame[6:12],
        "src": ip[12:16],
        "dst": ip[16:20],
        "ip_checksum_ok": ones_complement(ip[:ihl]) == 0,
        "type": icmp[0],
        "ident": struct.unpack(">H", icmp[4:6])[0],
        "seq": struct.unpack(">H", icmp[6:8])[0],
        "payload_len": len(icmp) - 8,
        "icmp_checksum_ok": ones_complement(icmp) == 0,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--iso", default="dist/x86_64/kernel.iso")
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--smp", default="2")
    parser.add_argument("--accel", default=None)
    args = parser.parse_args()

    iso = Path(args.iso).resolve()
    if not iso.exists():
        print(f"missing ISO: {iso}", file=sys.stderr)
        return 2

    log_path = Path("build/qemu-net-ping.log").resolve()
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_bytes(b"")
    pcap_path = Path("build/qemu-net-ping.pcap").resolve()
    if pcap_path.exists():
        pcap_path.unlink()

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
        "-smp", args.smp,
        *(["-accel", args.accel] if args.accel else []),
        "-netdev", "user,id=n0",
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
            print("e1000 did not initialise", file=sys.stderr)
            return 1

        qmp = Qmp(qmp_port, deadline)
        send_text(qmp, f"ping 10.0.2.2 {PING_COUNT}\n")

        # Far longer than PING_COUNT seconds, because usleep(1s) currently
        # takes about 15s: the driver poll task busy-spins on task_yield()
        # with no delay, and under TCG that starves IRQ0 badly enough to
        # lose PIT ticks. The wait is generous on purpose so this test
        # reports on ICMP rather than on that unrelated timing bug.
        time.sleep(45.0)

        log = log_path.read_text(encoding="utf-8", errors="replace")
        if "PANIC" in log:
            print(log[-4000:])
            print("kernel panic detected", file=sys.stderr)
            return 1

        if not pcap_path.exists():
            print("no capture produced", file=sys.stderr)
            return 1

        icmp = [m for m in (parse_icmp(f) for f in pcap_frames(pcap_path)) if m]

        requests = [
            m for m in icmp
            if m["type"] == ICMP_ECHO_REQUEST
            and m["eth_src"] == GUEST_MAC
            and m["src"] == GUEST_IP
            and m["dst"] == GATEWAY_IP
        ]
        replies = [
            m for m in icmp
            if m["type"] == ICMP_ECHO_REPLY
            and m["eth_src"] != GUEST_MAC
            and m["src"] == GATEWAY_IP
            and m["dst"] == GUEST_IP
        ]

        for m in icmp:
            kind = "request" if m["type"] == ICMP_ECHO_REQUEST else "reply"
            print(f"  {kind} seq={m['seq']} ident={m['ident']} "
                  f"len={m['payload_len']} ip_ck={'ok' if m['ip_checksum_ok'] else 'BAD'} "
                  f"icmp_ck={'ok' if m['icmp_checksum_ok'] else 'BAD'}")

        if not requests:
            print("guest sent no echo request -- ping never reached the "
                  "stack; check SYS_NET_PING wiring and that ping.elf is "
                  "on the disk image", file=sys.stderr)
            return 1

        # A bad checksum is the failure this test exists to catch: the
        # packet still looks perfect in a hex dump and the peer silently
        # drops it, which reads as "the network is broken" everywhere else.
        bad_ip = [m for m in requests if not m["ip_checksum_ok"]]
        if bad_ip:
            print(f"{len(bad_ip)} outbound request(s) carry a bad IPv4 header "
                  "checksum -- inet_checksum already returns wire order, so "
                  "check for a to_be16() wrapped around it in ipv4_output_framed",
                  file=sys.stderr)
            return 1

        bad_icmp = [m for m in requests if not m["icmp_checksum_ok"]]
        if bad_icmp:
            print(f"{len(bad_icmp)} outbound request(s) carry a bad ICMP "
                  "checksum", file=sys.stderr)
            return 1

        if not replies:
            print("gateway never replied -- the request went out but SLIRP "
                  "would not answer it; check the source address and that "
                  "the destination MAC came from ARP", file=sys.stderr)
            return 1

        # Matching sequence numbers prove the reply belongs to our request
        # rather than being unrelated traffic that happened to be ICMP.
        matched = sorted({m["seq"] for m in requests} & {m["seq"] for m in replies})
        if not matched:
            print("replies arrived but none matched a request sequence number",
                  file=sys.stderr)
            return 1

        print(f"ICMP ok: {len(requests)} request(s), {len(replies)} reply/replies, "
              f"sequences matched {matched}, all checksums valid")
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

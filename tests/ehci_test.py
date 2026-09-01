"""Boot TOS with QEMU EHCI and prove control plus periodic transfers work."""

from pathlib import Path
import re
import subprocess
import time

from kernel_panic_test import Qmp, available_port, wait_for_text


def main() -> None:
    iso = Path("dist/x86_64/kernel.iso").resolve()
    log = Path("build/qemu-ehci.log").resolve()
    log.write_bytes(b"")
    port = available_port()

    command = [
        "qemu-system-x86_64",
        "-machine", "q35",
        "-cdrom", str(iso),
        "-serial", f"file:{log}",
        "-display", "none",
        "-qmp", f"tcp:127.0.0.1:{port},server=on,wait=off",
        "-no-reboot",
        "-m", "256M",
        "-device", "usb-ehci,id=ehci",
        "-device", "usb-tablet,bus=ehci.0",
    ]

    process = subprocess.Popen(command, stdin=subprocess.DEVNULL)
    qmp = None
    try:
        deadline = time.monotonic() + 45
        if not wait_for_text(log, "EHCI: polling HID pointer endpoint", deadline):
            raise RuntimeError("EHCI did not enumerate the tablet")
        if not wait_for_text(log, "kernel booted", deadline):
            raise RuntimeError("kernel did not finish booting")

        qmp = Qmp(port, deadline)
        mice = qmp.command(
            "human-monitor-command", {"command-line": "info mice"}
        )["return"]
        match = re.search(r"Mouse #(\d+): QEMU (?:USB|HID) Tablet", mice)
        if not match:
            raise RuntimeError(f"QEMU did not expose the USB tablet: {mice!r}")
        qmp.command(
            "human-monitor-command",
            {"command-line": f"mouse_set {match.group(1)}"},
        )
        # HMP's relative mouse_move is enough for PS/2 tests, but a headless
        # absolute tablet consumes QEMU's structured absolute-axis events.
        qmp.command(
            "input-send-event",
            {
                "events": [
                    {"type": "abs", "data": {"axis": "x", "value": 20000}},
                    {"type": "abs", "data": {"axis": "y", "value": 10000}},
                    {"type": "btn", "data": {"button": "left", "down": True}},
                ]
            },
        )
        if not wait_for_text(log, "EHCI: report bytes", deadline):
            raise RuntimeError("EHCI periodic endpoint produced no input report")

        text = log.read_text(errors="replace")
        forbidden = ("page-fault details", "KERNEL PANIC", "host system error")
        for marker in forbidden:
            if marker in text:
                raise RuntimeError(f"unexpected EHCI boot failure: {marker}")
        print("ehci_test: control enumeration and periodic input passed")
    finally:
        if qmp is not None:
            qmp.close()
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


if __name__ == "__main__":
    main()

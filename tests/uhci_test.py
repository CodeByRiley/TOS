"""Boot TOS with QEMU UHCI and prove control plus interrupt transfers work.

The mirror of ehci_test.py for the other host controller. Both drivers now
enumerate through the shared device model in drivers/usb/usb_device.c, and
until this existed only the EHCI half of that was ever executed: every test
and tools/run.bat instantiate EHCI, and run.bat's UHCI line is commented out.
"""

from pathlib import Path
import re
import subprocess
import time

from kernel_panic_test import Qmp, available_port, wait_for_text


def main() -> None:
    iso = Path("dist/x86_64/kernel.iso").resolve()
    log = Path("build/qemu-uhci.log").resolve()
    log.write_bytes(b"")
    port = available_port()

    command = [
        "qemu-system-x86_64",
        # q35, like ehci_test.py. Not cosmetic: on the default i440fx machine
        # attaching *any* USB controller wedges the boot after late_init,
        # before "kernel booted" is logged. That predates the shared device
        # model - a pre-change build stalls identically, and EHCI on i440fx
        # stalls the same way - so it is a separate bug, not this test's
        # subject. It is why tools/run.bat's UHCI line is commented out.
        "-machine", "q35",
        "-cdrom", str(iso),
        "-serial", f"file:{log}",
        "-display", "none",
        "-qmp", f"tcp:127.0.0.1:{port},server=on,wait=off",
        "-no-reboot",
        "-m", "256M",
        "-device", "ich9-usb-uhci1,id=uhci",
        "-device", "usb-tablet,bus=uhci.0",
    ]

    process = subprocess.Popen(command, stdin=subprocess.DEVNULL)
    qmp = None
    try:
        deadline = time.monotonic() + 45
        if not wait_for_text(log, "UHCI: polling HID pointer endpoint", deadline):
            raise RuntimeError("UHCI did not enumerate the tablet")
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
        if not wait_for_text(log, "UHCI: report bytes", deadline):
            raise RuntimeError("UHCI interrupt endpoint produced no input report")

        text = log.read_text(errors="replace")
        forbidden = ("page-fault details", "KERNEL PANIC", "host system error")
        for marker in forbidden:
            if marker in text:
                raise RuntimeError(f"unexpected UHCI boot failure: {marker}")
        print("uhci_test: control enumeration and interrupt input passed")
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

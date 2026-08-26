from pathlib import Path
import subprocess
import time

from kernel_panic_test import Qmp, available_port, wait_for_text
from path_lookup_test import send_text


iso = Path("dist/x86_64/kernel.iso").resolve()
log = Path("build/qemu-netsurf-loading.log").resolve()
shot = Path("build/netsurf-loaded.ppm").resolve()
log.write_bytes(b"")
shot.unlink(missing_ok=True)
port = available_port()
command = [
    "qemu-system-x86_64",
    "-cdrom", str(iso),
    "-serial", f"file:{log}",
    "-display", "none",
    "-vga", "virtio",
    "-qmp", f"tcp:127.0.0.1:{port},server=on,wait=off",
    "-no-reboot",
    "-m", "256M",
    "-smp", "2",
]

process = subprocess.Popen(command, stdin=subprocess.DEVNULL)
qmp = None
try:
    deadline = time.monotonic() + 45
    if not wait_for_text(log, "winman: ready", deadline):
        raise RuntimeError("winman did not become ready")
    qmp = Qmp(port, deadline)
    send_text(qmp, "netsurf ")
    qmp.command("human-monitor-command", {"command-line": "sendkey minus"})
    send_text(qmp, "v\n")
    if not wait_for_text(log, "winman: create handle=", deadline):
        raise RuntimeError("NetSurf window was not created")
    time.sleep(8)
    qmp.command("screendump", {"filename": shot.as_posix()})
    print(shot)
finally:
    if qmp is not None:
        qmp.close()
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)

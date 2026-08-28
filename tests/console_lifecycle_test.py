#!/usr/bin/env python3
"""Open several Winman consoles, close every one, and keep going.

Covers the two things the multi-TTY console work added:

  * more than one shell at a time, each on its own kernel TTY channel
  * closing a console, including the last one

The second half is the interesting one. The kernel used to run the boot
shell with a blocking process_exec from its init task, so a shell that
exited without being respawned returned out of kernel_main and wedged the
machine. Closing every console and then opening a fresh one is exactly the
sequence that used to be fatal.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

from kernel_panic_test import Qmp, available_port, wait_for_text


def read_log(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def wait_for_count(path: Path, marker: str, count: int,
                   proc: subprocess.Popen, deadline: float) -> bool:
    """Wait until `marker` has appeared at least `count` times."""
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            return False
        if read_log(path).count(marker) >= count:
            return True
        time.sleep(0.05)
    return False


# A PS/2 movement packet carries a signed 8-bit delta per axis, so anything
# past ~127 pixels is clamped and the pointer lands short. Walk there instead.
MOUSE_STEP = 100


def mouse_move_to(qmp: Qmp, position: list[int], x: int, y: int) -> None:
    """Move the PS/2 pointer to an absolute screen position.

    HMP mouse_move is relative, so track where the pointer was left and send
    the delta in packet-sized steps.
    """
    while position[0] != x or position[1] != y:
        dx = max(-MOUSE_STEP, min(MOUSE_STEP, x - position[0]))
        dy = max(-MOUSE_STEP, min(MOUSE_STEP, y - position[1]))
        qmp.command("human-monitor-command",
                    {"command-line": f"mouse_move {dx} {dy}"})
        position[0] += dx
        position[1] += dy
        time.sleep(0.02)
    time.sleep(0.08)


def home_pointer(qmp: Qmp, position: list[int]) -> None:
    """Drive the pointer into the top-left corner so the position is known.

    The guest clamps at the screen edges, so overshooting is how the tracked
    position is re-synchronised with the real one after a drag or a click
    that moved focus.
    """
    for _ in range(16):
        qmp.command("human-monitor-command",
                    {"command-line": f"mouse_move {-MOUSE_STEP} {-MOUSE_STEP}"})
        time.sleep(0.02)
    position[0], position[1] = 0, 0
    time.sleep(0.15)


def click(qmp: Qmp) -> None:
    qmp.command("human-monitor-command", {"command-line": "mouse_button 1"})
    time.sleep(0.05)
    qmp.command("human-monitor-command", {"command-line": "mouse_button 0"})
    time.sleep(0.15)


def open_console_via_start_menu(qmp: Qmp, position: list[int],
                                fb_h: int, menu_items: int) -> None:
    """Click Start, then the pinned "Shelf (Shell)" entry at the top."""
    taskbar_y = fb_h - 24            # TASKBAR_PX
    home_pointer(qmp, position)
    mouse_move_to(qmp, position, 12, taskbar_y + 12)   # TASKBAR_START_W is 24
    click(qmp)

    menu_h = menu_items * 24 + 8     # START_MENU_ITEM_H, START_MENU_PAD * 2
    menu_y = taskbar_y - menu_h
    mouse_move_to(qmp, position, 80, menu_y + 4 + 12)  # first item, centred
    click(qmp)
    time.sleep(0.4)


def alt_f4(qmp: Qmp) -> None:
    qmp.command("human-monitor-command", {"command-line": "sendkey alt-f4"})
    time.sleep(0.35)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--iso", default="dist/x86_64/kernel.iso")
    parser.add_argument("--timeout", type=float, default=180)
    args = parser.parse_args()

    qemu = shutil.which(args.qemu) or args.qemu
    iso = Path(args.iso).resolve()
    if not iso.exists():
        print(f"missing ISO: {iso}", file=sys.stderr)
        return 2

    build = Path("build").resolve()
    build.mkdir(parents=True, exist_ok=True)
    log_path = build / "qemu-console-lifecycle.log"
    log_path.write_bytes(b"")

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
        "-smp", "2",
    ]

    proc = subprocess.Popen(command, stdin=subprocess.DEVNULL)
    qmp: Qmp | None = None
    try:
        deadline = time.monotonic() + args.timeout
        if not wait_for_text(log_path, "winman: ready", deadline):
            print(read_log(log_path))
            print("winman did not become ready", file=sys.stderr)
            return 1

        log = read_log(log_path)

        # The boot console is opened by winman now, not by the kernel, which
        # is what gives the close button a pid to kill.
        if "winman: console slot=0 tty=0 sh pid=" not in log:
            print(log)
            print("boot console did not open on tty 0", file=sys.stderr)
            return 1

        fb = re.search(r"winman: fb (\d+)x(\d+)", log)
        menu = re.search(r"winman: start menu (\d+) entries", log)
        if not fb or not menu:
            print(log)
            print("could not read framebuffer or start-menu geometry",
                  file=sys.stderr)
            return 1
        fb_h = int(fb.group(2))
        menu_items = int(menu.group(1))

        qmp = Qmp(qmp_port, deadline)
        position = [0, 0]

        # --- two more consoles, each on its own channel -------------------
        for expected_slot in (1, 2):
            open_console_via_start_menu(qmp, position, fb_h, menu_items)
            marker = f"winman: console slot={expected_slot} tty={expected_slot} sh pid="
            if not wait_for_count(log_path, marker, 1, proc, deadline):
                print(read_log(log_path))
                print(f"console slot {expected_slot} did not open",
                      file=sys.stderr)
                return 1

        # --- close all three, newest first --------------------------------
        # console_open focuses what it just opened, and Alt+F4 closes the
        # focused window, so this walks back down the stack.
        for expected_slot in (2, 1, 0):
            alt_f4(qmp)
            marker = f"winman: console slot={expected_slot} closed"
            if not wait_for_count(log_path, marker, 1, proc, deadline):
                print(read_log(log_path))
                print(f"console slot {expected_slot} did not close",
                      file=sys.stderr)
                return 1

        if proc.poll() is not None:
            print(read_log(log_path))
            print("guest died after closing the last console", file=sys.stderr)
            return 1

        # --- and the machine is still usable with zero shells open --------
        open_console_via_start_menu(qmp, position, fb_h, menu_items)
        if not wait_for_count(log_path, "winman: console slot=0 tty=0 sh pid=",
                              2, proc, deadline):
            print(read_log(log_path))
            print("could not reopen a console after closing every shell",
                  file=sys.stderr)
            return 1

        log = read_log(log_path)
        if "PANIC" in log:
            print(log)
            print("kernel panicked", file=sys.stderr)
            return 1

        print("opened 3 consoles on separate TTY channels, closed all 3, "
              "reopened one")
        return 0
    finally:
        if qmp is not None:
            qmp.close()
        if proc.poll() is None:
            proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)


if __name__ == "__main__":
    raise SystemExit(main())

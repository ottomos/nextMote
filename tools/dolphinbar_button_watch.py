#!/usr/bin/env python3
"""Read button-state reports from the already-connected DolphinBar slot only."""

import argparse
import os
import select
import time

VID = 0x057E
PID = 0x0306
STATUS_REQUEST = b"\x15\x00"
BUTTON_REPORT_ID = 0x30
# Core-button masks as documented by WiiBrew. The two button bytes are
# assembled little-endian: first byte is the low byte, second is the high byte.
BUTTON_NAMES = {
    0x0001: "LEFT",
    0x0002: "RIGHT",
    0x0004: "DOWN",
    0x0008: "UP",
    0x0010: "PLUS",
    0x0100: "TWO",
    0x0200: "ONE",
    0x0400: "B",
    0x0800: "A",
    0x1000: "MINUS",
    0x8000: "HOME",
}


def get_hidraw_node(hid_path: str) -> str:
    target = os.path.realpath(hid_path)
    for entry in os.listdir("/sys/class/hidraw"):
        candidate = f"/sys/class/hidraw/{entry}/device"
        if os.path.realpath(candidate) == target:
            return f"/dev/{entry}"
    raise RuntimeError(f"No hidraw node for {hid_path}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=float, default=15.0)
    args = parser.parse_args()

    hid_root = "/sys/bus/hid/devices"
    slots = sorted(
        name
        for name in os.listdir(hid_root)
        if name.startswith(f"0003:{VID:04X}:{PID:04X}.")
    )
    if len(slots) != 4:
        raise SystemExit(f"Expected 4 DolphinBar slots; found {len(slots)}")

    connected_node = None
    for slot in slots:
        node = get_hidraw_node(f"{hid_root}/{slot}")
        try:
            fd = os.open(node, os.O_RDWR | os.O_NONBLOCK)
            try:
                os.write(fd, STATUS_REQUEST)
                ready, _, _ = select.select([fd], [], [], 0.4)
                report = os.read(fd, 64) if ready else b""
            finally:
                os.close(fd)
        except OSError as exc:
            print(f"{slot}: slot unavailable ({exc.strerror})", flush=True)
            continue

        if report:
            print(f"Selected connected slot {slot} ({node}); initial report {report.hex(' ')}", flush=True)
            connected_node = (slot, node)
            break
        print(f"{slot}: empty/no status response", flush=True)

    if connected_node is None:
        raise SystemExit("No responding Wii Remote slot found; did not start a monitor")

    slot, node = connected_node
    fd = os.open(node, os.O_RDWR | os.O_NONBLOCK)
    try:
        os.write(fd, STATUS_REQUEST)
        deadline = time.monotonic() + max(1.0, args.seconds)
        seen = 0
        last_buttons = None
        print(f"Watching report 0x30 for up to {args.seconds:g}s; no pairing or button commands sent.", flush=True)
        while time.monotonic() < deadline:
            ready, _, _ = select.select([fd], [], [], min(0.25, deadline - time.monotonic()))
            if not ready:
                continue
            try:
                report = os.read(fd, 64)
            except BlockingIOError:
                continue
            except OSError as exc:
                print(f"Read stopped ({exc.strerror})", flush=True)
                break
            if not report:
                continue
            seen += 1
            if report[0] == BUTTON_REPORT_ID and len(report) >= 3:
                buttons = (report[2] << 8) | report[1]
                if buttons != last_buttons:
                    labels = [name for bit, name in BUTTON_NAMES.items() if buttons & bit]
                    print(f"Buttons: 0x{buttons:04x} {labels if labels else '[none]'}", flush=True)
                    last_buttons = buttons
            elif seen <= 3:
                print(f"Other input report {report[0]:02x}: {report.hex(' ')}", flush=True)
    finally:
        os.close(fd)
    print(f"Monitor finished: {seen} input report(s) from {slot}", flush=True)


if __name__ == "__main__":
    main()

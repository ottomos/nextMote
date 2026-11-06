#!/usr/bin/env python3
"""Repeatable DolphinBar SYNC acceptance test.

Read-only on the bar: snapshots the four USB HID slots before and after a SYNC
press and reports whether any slot became occupied. Optionally runs a Bluetooth
inquiry to record the Class of Device that the bar's discovery would see.

Nothing here pairs, scans on behalf of the bar, flashes firmware, or clears
bonds. Watch the ESP32 serial monitor at the same time for the firmware side.
"""

import argparse
import errno
import os
import re
import select
import subprocess
import sys
import time

VID = 0x057E
PID = 0x0306
STATUS_REQUEST = b"\x15\x00"
SLOT_TIMEOUT = 0.6

WIIMOTE_COD = 0x002504
INQUIRY_RE = re.compile(
    r"((?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}).*?class:\s*(0x[0-9A-Fa-f]{6})"
)


def decode_cod(cod: int) -> str:
    service = (cod >> 13) & 0x7FF
    major = (cod >> 8) & 0x1F
    minor = (cod >> 2) & 0x3F
    limited = "limited-discoverable" if cod & 0x2000 else "not-limited-discoverable"
    return f"service=0x{service:03x} major=0x{major:02x} minor=0x{minor:02x} {limited}"


def hidraw_for(hid_path: str) -> str:
    target = os.path.realpath(hid_path)
    for entry in os.listdir("/sys/class/hidraw"):
        if os.path.realpath(f"/sys/class/hidraw/{entry}/device") == target:
            return f"/dev/{entry}"
    raise RuntimeError(f"No hidraw node for {hid_path}")


def find_slots() -> list[tuple[str, str]]:
    root = "/sys/bus/hid/devices"
    names = sorted(
        name for name in os.listdir(root) if name.startswith(f"0003:{VID:04X}:{PID:04X}.")
    )
    if not names:
        raise SystemExit(
            "No DolphinBar HID slots found. Is the bar plugged in and in mode 4?"
        )
    return [(name, hidraw_for(f"{root}/{name}")) for name in names]


def probe_slot(node: str) -> tuple[str, bytes]:
    """Return (state, report). state is occupied / empty / error."""
    try:
        fd = os.open(node, os.O_RDWR | os.O_NONBLOCK)
    except OSError as exc:
        return f"error: {exc.strerror}", b""
    try:
        try:
            os.write(fd, STATUS_REQUEST)
        except OSError as exc:
            if exc.errno == errno.EPIPE:
                return "empty", b""
            return f"error: {exc.strerror}", b""
        ready, _, _ = select.select([fd], [], [], SLOT_TIMEOUT)
        if not ready:
            return "empty", b""
        try:
            report = os.read(fd, 64)
        except OSError as exc:
            if exc.errno == errno.EPIPE:
                return "empty", b""
            return f"error: {exc.strerror}", b""
        return ("occupied", report) if report else ("empty", b"")
    finally:
        os.close(fd)


def snapshot(label: str) -> dict[str, str]:
    print(f"\n--- bar slots: {label} ---", flush=True)
    states: dict[str, str] = {}
    for slot, node in find_slots():
        state, report = probe_slot(node)
        states[slot] = state
        detail = f"  report {report.hex(' ')}" if report else ""
        print(f"  {slot} ({node}): {state}{detail}", flush=True)
    return states


def inquiry(expect: str | None) -> None:
    print("\n--- Bluetooth inquiry (laptop adapter, read-only) ---", flush=True)
    print("    Note: this is the laptop looking, NOT the DolphinBar.", flush=True)
    try:
        proc = subprocess.run(
            ["hcitool", "inq", "--flush"], capture_output=True, text=True, timeout=25
        )
    except FileNotFoundError:
        print("  hcitool not available; skipping inquiry", flush=True)
        return
    except subprocess.TimeoutExpired:
        print("  inquiry timed out", flush=True)
        return

    found = INQUIRY_RE.findall(proc.stdout)
    if not found:
        print("  no devices answered the inquiry", flush=True)
    for addr, cod_text in found:
        cod = int(cod_text, 16)
        marks = []
        if expect and addr.lower() == expect.lower():
            marks.append("<= ESP32 under test")
        if cod == WIIMOTE_COD:
            marks.append("matches real Wii Remote CoD")
        elif (cod & 0x1FFF) == (WIIMOTE_COD & 0x1FFF):
            marks.append(f"CoD differs from Wii Remote 0x{WIIMOTE_COD:06x}")
        suffix = f"   {' · '.join(marks)}" if marks else ""
        print(f"  {addr}  {cod_text}  {decode_cod(cod)}{suffix}", flush=True)

    if expect and not any(a.lower() == expect.lower() for a, _ in found):
        print(
            f"  {expect} did NOT answer the inquiry -> it is not discoverable right now",
            flush=True,
        )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--esp-addr",
        default=None,
        help="ESP32 Bluetooth address to flag in inquiry results (optional)",
    )
    parser.add_argument(
        "--no-inquiry", action="store_true", help="Skip the laptop-side inquiry step"
    )
    parser.add_argument(
        "--wait", type=float, default=25.0, help="Seconds to wait after the SYNC prompt"
    )
    args = parser.parse_args()

    print("DolphinBar SYNC acceptance test", flush=True)
    print(f"UTC start: {time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}", flush=True)
    print("Keep the ESP32 serial monitor visible in another window.", flush=True)

    before = snapshot("before SYNC")
    if not args.no_inquiry:
        inquiry(args.esp_addr)

    print(f"\n>>> PRESS THE DOLPHINBAR SYNC BUTTON NOW. Waiting {args.wait:g}s...", flush=True)
    deadline = time.monotonic() + args.wait
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        print(f"    {remaining:4.0f}s remaining", end="\r", flush=True)
        time.sleep(min(1.0, remaining))
    print(" " * 30, end="\r")

    after = snapshot("after SYNC")

    print("\n=== result ===", flush=True)
    changed = [s for s in after if before.get(s) != after[s]]
    occupied = [s for s, state in after.items() if state == "occupied"]
    if changed:
        for slot in changed:
            print(f"  CHANGED {slot}: {before.get(slot)} -> {after[slot]}", flush=True)
    else:
        print("  No slot changed state.", flush=True)
    if occupied:
        print(f"  Occupied slot(s): {', '.join(occupied)}", flush=True)
        print("  Confirm on serial whether the ESP32 logged the HID open event.", flush=True)
    else:
        print("  No bar slot is occupied: no remote (real or emulated) is connected.", flush=True)
    print("  Bar-side slot state alone is not proof; cross-check the serial log.", flush=True)
    print(f"UTC end: {time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}", flush=True)


if __name__ == "__main__":
    sys.exit(main())

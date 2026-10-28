#!/usr/bin/env python3
"""Live cold-boot connection test: ESP32 serial + DolphinBar slots in one view.

Start this BEFORE powering anything on. It waits for the ESP32 serial port and
the DolphinBar to appear, then streams the ESP32's own log next to the bar's USB
slot state so you can watch a connection form in real time.

Read-only on the bar apart from the Wii Remote status request (0x15) used to
tell an occupied slot from an empty one. It never pairs, flashes, or clears
bonds. Close Dolphin before running: it claims the same hidraw nodes.
"""

import argparse
import errno
import glob
import os
import re
import select
import sys
import threading
import time

VID = 0x057E
PID = 0x0306
STATUS_REQUEST = b"\x15\x00"
SLOT_TIMEOUT = 0.4

C = {
    "reset": "\033[0m",
    "dim": "\033[2m",
    "bold": "\033[1m",
    "red": "\033[31m",
    "green": "\033[32m",
    "yellow": "\033[33m",
    "blue": "\033[34m",
    "cyan": "\033[36m",
}

# Ordered milestones. Each entry: key, human label, serial regex (or None when
# the stage is satisfied by bar-side evidence instead).
STAGES = [
    ("boot", "ESP32 booted", re.compile(r"ESP32 WROOM Classic Bluetooth HID peripheral")),
    ("ready", "Advertising, discoverable", re.compile(r"READY identity|connectable, \w+ discoverable")),
    ("pin", "Pairing started (PIN request)", re.compile(r"legacy PIN request")),
    ("auth", "Authenticated with host", re.compile(r"authentication status=0")),
    ("hid", "HID channels open", re.compile(r"WIIREMOTE_HID_CONNECTED")),
    ("config", "Host configured the session", re.compile(r"report mode=|LED state=")),
    ("slot", "Bar slot occupied (USB side)", None),
]

HIGHLIGHT = [
    (re.compile(r"WIIREMOTE_HID_CONNECTED|authentication status=0"), "green"),
    (re.compile(r"legacy PIN|report mode=|LED state=|CoD="), "cyan"),
    (re.compile(r"authentication status=[1-9]|HID close|disconnect|unplug"), "red"),
    (re.compile(r"^W \("), "yellow"),
    (re.compile(r"^E \("), "red"),
]

NOISE = re.compile(r"BT_HCI: (hci cmd send: sniff|opcode=0x0803)")


class Tracker:
    def __init__(self) -> None:
        self.done: dict[str, float] = {}
        self.start = time.monotonic()
        self.lock = threading.Lock()

    def mark(self, key: str) -> bool:
        with self.lock:
            if key in self.done:
                return False
            self.done[key] = time.monotonic() - self.start
            return True


def paint(text: str, colour: str, enabled: bool) -> str:
    return f"{C[colour]}{text}{C['reset']}" if enabled else text


def stamp() -> str:
    return time.strftime("%H:%M:%S")


def find_serial(explicit: str | None) -> str | None:
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for pattern in ("/dev/serial/by-id/*CP2102*", "/dev/ttyUSB*", "/dev/ttyACM*"):
        hits = sorted(glob.glob(pattern))
        if hits:
            return hits[0]
    return None


def hidraw_for(hid_path: str) -> str | None:
    target = os.path.realpath(hid_path)
    try:
        entries = os.listdir("/sys/class/hidraw")
    except OSError:
        return None
    for entry in entries:
        if os.path.realpath(f"/sys/class/hidraw/{entry}/device") == target:
            return f"/dev/{entry}"
    return None


def find_slots() -> list[tuple[str, str]]:
    root = "/sys/bus/hid/devices"
    try:
        names = sorted(n for n in os.listdir(root) if n.startswith(f"0003:{VID:04X}:{PID:04X}."))
    except OSError:
        return []
    slots = []
    for name in names:
        node = hidraw_for(f"{root}/{name}")
        if node:
            slots.append((name, node))
    return slots


def probe_slot(node: str) -> tuple[str, bytes]:
    try:
        fd = os.open(node, os.O_RDWR | os.O_NONBLOCK)
    except OSError as exc:
        return ("empty", b"") if exc.errno in (errno.EPIPE, errno.ENODEV) else (f"error:{exc.errno}", b"")
    try:
        try:
            os.write(fd, STATUS_REQUEST)
        except OSError as exc:
            return ("empty", b"") if exc.errno == errno.EPIPE else (f"error:{exc.errno}", b"")
        ready, _, _ = select.select([fd], [], [], SLOT_TIMEOUT)
        if not ready:
            return "empty", b""
        try:
            report = os.read(fd, 64)
        except OSError as exc:
            return ("empty", b"") if exc.errno == errno.EPIPE else (f"error:{exc.errno}", b"")
        return ("occupied", report) if report else ("empty", b"")
    finally:
        os.close(fd)


def serial_thread(port: str, baud: float, tracker: Tracker, colour: bool, verbose: bool,
                  stop: threading.Event) -> None:
    try:
        import serial
    except ImportError:
        print(paint("  pyserial not installed; serial side unavailable", "red", colour), flush=True)
        return

    handle = None
    while not stop.is_set():
        if handle is None:
            try:
                handle = serial.Serial(port, baud, timeout=0.5)
                print(paint(f"[{stamp()}] SERIAL  opened {port} @ {baud:g}", "blue", colour), flush=True)
            except Exception as exc:
                print(paint(f"[{stamp()}] SERIAL  waiting for {port} ({exc.__class__.__name__})",
                            "dim", colour), flush=True)
                stop.wait(2.0)
                continue

        try:
            raw = handle.readline()
        except Exception:
            print(paint(f"[{stamp()}] SERIAL  port dropped; will reopen", "yellow", colour), flush=True)
            try:
                handle.close()
            except Exception:
                pass
            handle = None
            continue

        if not raw:
            continue
        line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
        if not line:
            continue

        for key, label, pattern in STAGES:
            if pattern and pattern.search(line) and tracker.mark(key):
                print(paint(f"[{stamp()}] STAGE   {label}", "bold", colour)
                      + paint("  <= milestone", "green", colour), flush=True)

        if NOISE.search(line) and not verbose:
            continue

        colour_name = "dim"
        for pattern, name in HIGHLIGHT:
            if pattern.search(line):
                colour_name = name
                break
        print(paint(f"[{stamp()}] ESP32   {line}", colour_name, colour), flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", help="Serial port (default: auto-detect CP2102/ttyUSB)")
    parser.add_argument("--baud", type=float, default=115200)
    parser.add_argument("--interval", type=float, default=3.0, help="Seconds between bar slot polls")
    parser.add_argument("--seconds", type=float, default=300.0, help="Total run time")
    parser.add_argument("--verbose", action="store_true", help="Include noisy HCI sniff lines")
    parser.add_argument("--no-color", action="store_true")
    args = parser.parse_args()
    colour = not args.no_color and sys.stdout.isatty()

    print(paint("ESPmote live connection test", "bold", colour), flush=True)
    print(f"UTC start {time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}", flush=True)
    print(paint("Power on the DolphinBar (mode 4) and the ESP32 now, then press SYNC.",
                "cyan", colour), flush=True)
    print(paint("Close Dolphin while this runs: it claims the same hidraw nodes.\n",
                "dim", colour), flush=True)

    tracker = Tracker()
    stop = threading.Event()

    port = find_serial(args.port)
    while port is None and not stop.is_set():
        print(paint(f"[{stamp()}] SERIAL  no ESP32 serial port yet; waiting...", "dim", colour), flush=True)
        time.sleep(2.0)
        port = find_serial(args.port)
        if time.monotonic() - tracker.start > args.seconds:
            break

    reader = None
    if port:
        reader = threading.Thread(target=serial_thread,
                                  args=(port, args.baud, tracker, colour, args.verbose, stop),
                                  daemon=True)
        reader.start()

    previous: dict[str, str] = {}
    announced_bar = False
    deadline = tracker.start + args.seconds
    try:
        while time.monotonic() < deadline:
            slots = find_slots()
            if not slots:
                if announced_bar:
                    print(paint(f"[{stamp()}] BAR     DolphinBar disappeared from USB", "yellow", colour),
                          flush=True)
                    announced_bar = False
                    previous = {}
                else:
                    print(paint(f"[{stamp()}] BAR     waiting for DolphinBar on USB...", "dim", colour),
                          flush=True)
                time.sleep(args.interval)
                continue

            if not announced_bar:
                print(paint(f"[{stamp()}] BAR     DolphinBar present: {len(slots)} slots", "blue", colour),
                      flush=True)
                announced_bar = True

            for index, (slot, node) in enumerate(slots, start=1):
                state, report = probe_slot(node)
                if previous.get(slot) == state:
                    continue
                previous[slot] = state
                detail = f"  report {report.hex(' ')}" if report else ""
                tone = "green" if state == "occupied" else "dim"
                print(paint(f"[{stamp()}] BAR     slot {index} ({node}): {state}{detail}", tone, colour),
                      flush=True)
                if state == "occupied" and tracker.mark("slot"):
                    print(paint(f"[{stamp()}] STAGE   Bar slot occupied (USB side)", "bold", colour)
                          + paint("  <= milestone", "green", colour), flush=True)

            time.sleep(args.interval)
    except KeyboardInterrupt:
        print("\ninterrupted", flush=True)
    finally:
        stop.set()
        if reader:
            reader.join(timeout=2.0)

    print(paint("\n=== milestone summary ===", "bold", colour), flush=True)
    for key, label, _ in STAGES:
        if key in tracker.done:
            print(paint(f"  [x] {label}  (+{tracker.done[key]:.1f}s)", "green", colour), flush=True)
        else:
            print(paint(f"  [ ] {label}", "red", colour), flush=True)

    required = ("pin", "auth", "hid", "slot")
    if all(k in tracker.done for k in required):
        print(paint("\nPASS: paired, HID opened, and the bar presented an occupied slot.",
                    "green", colour), flush=True)
        print("To prove which slot is the ESP32, write an LED pattern to that slot and", flush=True)
        print("confirm the ESP32 logs the same value, e.g.:", flush=True)
        print(paint("  printf '\\x11\\xf0' > /dev/hidrawN", "cyan", colour), flush=True)
        return 0

    print(paint("\nINCOMPLETE: see unchecked stages above.", "yellow", colour), flush=True)
    if "pin" not in tracker.done:
        print("  No PIN request means the host never contacted the ESP32 -> a discovery", flush=True)
        print("  problem (Class of Device / discoverable mode), not a pairing problem.", flush=True)
    elif "hid" not in tracker.done:
        print("  Pairing began but HID never opened -> check the SDP record and Device ID.", flush=True)
    return 1


if __name__ == "__main__":
    sys.exit(main())

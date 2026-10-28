#!/usr/bin/env python3
"""Read only the Nintendo vendor HID status reports from DolphinBar slots."""

import errno
import os
import select
import struct
import time


VID = 0x057E
PID = 0x0306
STATUS_REPORT = b"\x15\x00"
STATUS_TIMEOUT = 2.0


def hidraw_for(hid_path: str) -> str:
    target = os.path.realpath(hid_path)
    for entry in os.listdir("/sys/class/hidraw"):
        candidate = os.path.join("/sys/class/hidraw", entry, "device")
        if os.path.realpath(candidate) == target:
            return f"/dev/{entry}"
    raise RuntimeError(f"Could not find hidraw node for {hid_path}")


def main() -> None:
    root = "/sys/bus/hid/devices"
    matches = sorted(
        name
        for name in os.listdir(root)
        if name.startswith(f"0003:{VID:04X}:{PID:04X}.")
    )
    if len(matches) != 4:
        raise SystemExit(f"Expected 4 DolphinBar HID slots, found {len(matches)}")

    print("DolphinBar USB 057e:0306 · passive connected-remote probe", flush=True)
    print("Command: HID SET_REPORT status request 15 00; no scan/sync/pairing", flush=True)
    print(f"UTC run start: {time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}", flush=True)

    for hid_id in matches:
        hidraw = hidraw_for(f"{root}/{hid_id}")
        descriptor = open(hidraw, "r+b", buffering=0)
        print(f"\n{hid_id} ({hidraw})", flush=True)

        try:
            written = descriptor.write(STATUS_REPORT)
            print(f"  status request wrote {written} bytes", flush=True)
        except OSError as exc:
            print(f"  WRITE ERROR errno={exc.errno}: {exc.strerror}", flush=True)
            descriptor.close()
            continue

        ready, _, _ = select.select([descriptor], [], [], STATUS_TIMEOUT)
        if not ready:
            print("  status response: timeout (no remote may occupy this slot)", flush=True)
        else:
            try:
                report = descriptor.read(64)
                print(f"  status response ({len(report)} bytes): {report.hex(' ')}", flush=True)
            except OSError as exc:
                classification = "absent-slot EPIPE" if exc.errno == errno.EPIPE else "read error"
                print(f"  status response: {classification} errno={exc.errno}: {exc.strerror}", flush=True)
        descriptor.close()

    print(f"\nUTC run end: {time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}", flush=True)


if __name__ == "__main__":
    main()

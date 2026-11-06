#!/usr/bin/env python3
"""Compare actual HID startup/read-response sequences from wiimote and nextMote captures."""

import argparse
from pathlib import Path

from read_wiimote_reference import memory_reply


def captured_sequence(output: str, address: int) -> list[bytes]:
    frames = []
    for line in output.splitlines():
        if " RX " not in line:
            continue
        frame = bytes.fromhex(line.split(" RX ", 1)[1])
        frames.append(frame)
        if memory_reply(frame, address):
            return frames
    raise ValueError(f"Capture has no complete response for EEPROM 0x{address:04x}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("wiimote", type=Path)
    parser.add_argument("next_mote", type=Path)
    parser.add_argument("--address", type=lambda value: int(value, 0), default=0x1770)
    args = parser.parse_args()
    wiimote = captured_sequence(args.wiimote.read_text(encoding="utf-8"), args.address)
    next_mote = captured_sequence(args.next_mote.read_text(encoding="utf-8"), args.address)
    if wiimote != next_mote:
        print("wiimote reports:", [f"0x{frame[1]:02x}" for frame in wiimote if len(frame) >= 2])
        print("nextMote reports:", [f"0x{frame[1]:02x}" for frame in next_mote if len(frame) >= 2])
        print("FAIL: startup/read sequence or response bytes differ")
        return 1
    print(
        f"PASS: all {len(wiimote)} startup/read frames and EEPROM "
        f"0x{args.address:04x} response bytes match"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

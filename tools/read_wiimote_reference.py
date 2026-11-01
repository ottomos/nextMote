#!/usr/bin/env python3
"""Capture one read-only EEPROM response within a single wiimote SYNC window."""

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import socket
import time

CAPTURE_BUDGET_SECONDS = 16
CONNECT_TIMEOUT_SECONDS = 6
REPLY_TIMEOUT_SECONDS = 3


@dataclass(frozen=True)
class MemoryReply:
    raw: bytes
    address: int
    error: int
    size_minus_one: int
    data: bytes


def eeprom_request(address: int) -> bytes:
    if not 0 <= address <= 0xFFFF:
        raise ValueError("EEPROM address must fit in two bytes")
    return bytes((0xA2, 0x17, 0, 0, address >> 8, address & 0xFF, 0, 1))


def memory_reply(frame: bytes, address: int) -> MemoryReply | None:
    if frame[:2] != b"\xa1\x21":
        return None
    if len(frame) < 7:
        raise ValueError(f"Memory reply is too short to decode: {frame.hex(' ')}")
    reported_address = int.from_bytes(frame[5:7], "big")
    if reported_address != address:
        return None
    return MemoryReply(
        raw=frame,
        address=reported_address,
        error=frame[4] & 0x0F,
        size_minus_one=frame[4] >> 4,
        data=frame[7:],
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bluetooth_address")
    parser.add_argument("--address", type=lambda value: int(value, 0), default=0x1770)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--label", choices=("wiimote", "espMote"), default="wiimote")
    args = parser.parse_args()
    if not re.fullmatch(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}", args.bluetooth_address):
        parser.error("Bluetooth address must contain six colon-separated hex bytes")
    request = eeprom_request(args.address)
    started = time.monotonic()
    deadline = started + CAPTURE_BUDGET_SECONDS

    def remaining(maximum: float) -> float:
        left = deadline - time.monotonic()
        if left <= 0:
            raise TimeoutError("The bounded reference-capture budget expired")
        return min(left, maximum)

    with args.output.open("w", encoding="utf-8") as output:
        def log(text: str) -> None:
            line = f"{time.monotonic() - started:.3f}s {args.label}: {text}"
            print(line, flush=True)
            output.write(line + "\n")
            output.flush()

        with (
            socket.socket(socket.AF_BLUETOOTH, socket.SOCK_SEQPACKET, socket.BTPROTO_L2CAP) as control,
            socket.socket(socket.AF_BLUETOOTH, socket.SOCK_SEQPACKET, socket.BTPROTO_L2CAP) as interrupt,
        ):
            log("opening HID control PSM 0x11")
            control.settimeout(remaining(CONNECT_TIMEOUT_SECONDS))
            control.connect((args.bluetooth_address, 0x11))
            log("control connected; opening interrupt PSM 0x13")
            interrupt.settimeout(remaining(CONNECT_TIMEOUT_SECONDS))
            interrupt.connect((args.bluetooth_address, 0x13))
            interrupt.settimeout(remaining(2))
            log("TX " + request.hex(" "))
            interrupt.sendall(request)
            reply_deadline = min(deadline, time.monotonic() + REPLY_TIMEOUT_SECONDS)
            while time.monotonic() < reply_deadline:
                reply_remaining = reply_deadline - time.monotonic()
                if reply_remaining <= 0:
                    break
                interrupt.settimeout(min(remaining(REPLY_TIMEOUT_SECONDS),
                                         reply_remaining))
                frame = interrupt.recv(64)
                if not frame:
                    raise ConnectionError("HID interrupt channel closed before the requested reply")
                log("RX " + frame.hex(" "))
                reply = memory_reply(frame, args.address)
                if reply:
                    log(f"CAPTURED address=0x{reply.address:04x} length={len(reply.raw)} "
                        f"error={reply.error} size_minus_one={reply.size_minus_one}")
                    if len(reply.raw) != 23:
                        log("NOTICE: actual reply length differs from the 23-byte report descriptor")
                    return 0
            raise TimeoutError("No matching EEPROM response within the bounded reply window")


if __name__ == "__main__":
    raise SystemExit(main())

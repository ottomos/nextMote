#!/usr/bin/env python3
"""Measure inquiry/paging fields omitted by hcitool without disturbing an active nextMote."""

import argparse
import ctypes
from ctypes.util import find_library
from dataclasses import dataclass
import json
import os
import re
import sys
import time
import urllib.request

TEST_DIR = os.path.join(os.path.dirname(os.path.dirname(__file__)), "test")
if TEST_DIR not in sys.path:
    sys.path.insert(0, TEST_DIR)

from nextmote_discovery_test import open_pairing_window

INQUIRY_INFO_SIZE = 14
MAX_RESPONSES = 20
CACHE_FLUSH = 0x0001
MANUAL_CAPTURE_BUDGET = 17


class InquiryInfo(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("address", ctypes.c_uint8 * 6),
        ("page_scan_repetition_mode", ctypes.c_uint8),
        ("page_scan_period_mode", ctypes.c_uint8),
        ("page_scan_mode", ctypes.c_uint8),
        ("device_class", ctypes.c_uint8 * 3),
        ("clock_offset", ctypes.c_uint16),
    ]


@dataclass(frozen=True)
class InquiryResult:
    address: str
    device_class: int
    page_scan_repetition_mode: int
    page_scan_period_mode: int
    page_scan_mode: int
    clock_offset: int


def parse_inquiry_info(data: bytes) -> InquiryResult:
    if len(data) != INQUIRY_INFO_SIZE:
        raise ValueError(f"Expected {INQUIRY_INFO_SIZE} inquiry bytes, got {len(data)}")
    return InquiryResult(
        address=":".join(f"{value:02x}" for value in data[:6][::-1]),
        device_class=int.from_bytes(data[9:12], "little"),
        page_scan_repetition_mode=data[6],
        page_scan_period_mode=data[7],
        page_scan_mode=data[8],
        clock_offset=int.from_bytes(data[12:14], "little"),
    )


def load_library() -> ctypes.CDLL:
    name = find_library("bluetooth")
    if not name:
        raise RuntimeError("The existing libbluetooth library is unavailable")
    if ctypes.sizeof(InquiryInfo) != INQUIRY_INFO_SIZE:
        raise RuntimeError("Native inquiry ABI size does not match BlueZ")
    library = ctypes.CDLL(name, use_errno=True)
    info_pointer = ctypes.POINTER(InquiryInfo)
    library.hci_inquiry.argtypes = [
        ctypes.c_int, ctypes.c_int, ctypes.c_int,
        ctypes.POINTER(ctypes.c_uint8), ctypes.POINTER(info_pointer), ctypes.c_long,
    ]
    library.hci_inquiry.restype = ctypes.c_int
    library.bt_free.argtypes = [ctypes.c_void_p]
    library.bt_free.restype = None
    return library


def run_inquiry(device_id: int, length: int, limited: bool) -> list[InquiryResult]:
    library = load_library()
    lap = (ctypes.c_uint8 * 3)(0x00 if limited else 0x33, 0x8B, 0x9E)
    info = ctypes.POINTER(InquiryInfo)()
    count = library.hci_inquiry(
        device_id, length, MAX_RESPONSES, lap, ctypes.byref(info), CACHE_FLUSH
    )
    try:
        if count < 0:
            error = ctypes.get_errno()
            if error == 0:
                raise RuntimeError("Native inquiry failed without an errno")
            raise OSError(error, os.strerror(error))
        if count > MAX_RESPONSES or (count and not info):
            raise RuntimeError("Native inquiry returned an invalid result buffer/count")
        return [
            parse_inquiry_info(ctypes.string_at(ctypes.addressof(info[i]), INQUIRY_INFO_SIZE))
            for i in range(count)
        ]
    finally:
        if info:
            library.bt_free(ctypes.cast(info, ctypes.c_void_p))


def require_idle_next_mote(base_url: str) -> None:
    with urllib.request.urlopen(f"{base_url.rstrip('/')}/api/status", timeout=8) as response:
        status = json.load(response)
    if status.get("wifi") is not True:
        raise RuntimeError(f"nextMote Wi-Fi is not ready: {status!r}")
    if status.get("connected") is not False:
        raise RuntimeError("nextMote is in use or connection state is unknown; refusing active inquiry/SYNC")


def check_manual_budget(cycles: int, length: int) -> None:
    if cycles * length * 1.28 > MANUAL_CAPTURE_BUDGET:
        raise ValueError("Manual wiimote capture exceeds the safe portion of its 20-second SYNC window")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target_address")
    parser.add_argument("--interface", default="hci0")
    parser.add_argument("--label", choices=("wiimote", "nextMote"), default="wiimote")
    parser.add_argument("--iac", choices=("general", "limited"), default="general")
    parser.add_argument("--length", type=int, default=2, help="inquiry length in 1.28-second units (1-12)")
    parser.add_argument("--cycles", type=int, default=1, help="bounded cycles (1-10)")
    parser.add_argument("--base-url", help="open nextMote SYNC automatically, only when disconnected")
    args = parser.parse_args()
    if not re.fullmatch(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}", args.target_address):
        parser.error("Target must be a six-byte Bluetooth address")
    interface = re.fullmatch(r"hci(\d+)", args.interface)
    if not interface or int(interface.group(1)) > 0xFFFF:
        parser.error("Interface must be hciN with a valid numeric device ID")
    if not 1 <= args.length <= 12 or not 1 <= args.cycles <= 10:
        parser.error("Length/cycles are outside the bounded diagnostic range")
    if args.base_url and args.label != "nextMote":
        parser.error("--base-url is only for nextMote, not the wiimote reference")
    if not args.base_url:
        try:
            check_manual_budget(args.cycles, args.length)
        except ValueError as error:
            parser.error(str(error))

    target = args.target_address.lower()
    successes = 0
    for cycle in range(1, args.cycles + 1):
        if args.base_url:
            require_idle_next_mote(args.base_url)
            open_pairing_window(args.base_url, "/api/sync", allow_giac_only=True,
                                allow_upstream_continuous=True)
        started = time.monotonic()
        results = run_inquiry(int(interface.group(1)), args.length, args.iac == "limited")
        matching = [result for result in results if result.address == target]
        if matching:
            successes += 1
        for result in matching:
            print(json.dumps({
                "label": args.label,
                "cycle": cycle,
                "iac": "LIAC" if args.iac == "limited" else "GIAC",
                "length_units": args.length,
                "elapsed_seconds": round(time.monotonic() - started, 3),
                "address": result.address,
                "class": f"0x{result.device_class:06x}",
                "page_scan_repetition_mode": result.page_scan_repetition_mode,
                "page_scan_period_mode": result.page_scan_period_mode,
                "page_scan_mode": result.page_scan_mode,
                "clock_offset_raw": f"0x{result.clock_offset:04x}",
            }), flush=True)
        if not matching:
            print(f"{args.label} cycle {cycle}: target not seen ({args.iac}, {args.length} units)",
                  flush=True)
    print(f"Measured {args.label} visibility: {successes}/{args.cycles}; inquiry only, no keys read or removed")
    return 0 if successes == args.cycles else 1


if __name__ == "__main__":
    raise SystemExit(main())

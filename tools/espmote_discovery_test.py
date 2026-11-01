#!/usr/bin/env python3
"""Repeat bounded BR/EDR inquiry checks during ESPmote SYNC windows."""

import argparse
import json
import re
import subprocess
import time
import urllib.request

ESP_MOTE_ADDRESS = "e4:65:b8:71:6e:12"
EXPECTED_CLASSES = {
    "sync": 0x002504,
    "general": 0x000504,
    "limited": 0x002504,
}
MAX_CYCLES = 10

DISCOVERY_TESTS = {
    "sync": (
        "/api/sync",
        (("GIAC", (), 6), ("LIAC", ("--iac=0x9e8b00",), 6)),
    ),
    "general": (
        "/api/sync/general",
        (("GIAC", (), 12),),
    ),
    "limited": (
        "/api/sync/limited",
        (("GIAC", (), 6), ("LIAC", ("--iac=0x9e8b00",), 6)),
    ),
}


def parse_inquiry_results(output: str) -> list[tuple[str, int]]:
    devices = []
    for line in output.splitlines():
        match = re.match(
            r"\s*([0-9A-Fa-f:]{17})\s+clock offset:\s*0x[0-9A-Fa-f]+\s+class:\s*0x([0-9A-Fa-f]+)",
            line,
        )
        if match:
            devices.append((match.group(1).lower(), int(match.group(2), 16)))
    return devices


def open_pairing_window(base_url: str, endpoint: str, *, allow_giac_only: bool = False,
                        allow_upstream_continuous: bool = False) -> int:
    request = urllib.request.Request(
        f"{base_url.rstrip('/')}{endpoint}",
        data=b"",
        headers={"Content-Type": "application/x-www-form-urlencoded"},
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=8) as response:
        result = json.load(response)
    if result.get("ok") is not True:
        raise RuntimeError(f"ESPmote refused the SYNC request: {result!r}")
    window_seconds = result.get("window_seconds")
    if result.get("pairing_profile") == "upstream-general-continuous":
        if not allow_upstream_continuous:
            raise RuntimeError("Upstream continuous General profile is active; timed Limited parity is not being tested")
        if not isinstance(window_seconds, int) or window_seconds != 0 or result.get("discovery") != "general":
            raise RuntimeError(f"Unexpected upstream discovery profile response: {result!r}")
        return 0
    if not isinstance(window_seconds, int) or window_seconds != 30:
        raise RuntimeError(f"Unexpected ESPmote SYNC window length: {window_seconds!r}")
    expected_discovery = "general" if endpoint == "/api/sync/general" else "limited"
    if result.get("discovery") != expected_discovery:
        raise RuntimeError(f"Unexpected ESPmote discovery mode: {result!r}")
    if result.get("diagnostic_giac_only") is True and not allow_giac_only:
        raise RuntimeError("GIAC-only diagnostic is active; full discovery parity cannot be tested")
    return window_seconds


def check_web_server(base_url: str) -> None:
    with urllib.request.urlopen(f"{base_url.rstrip('/')}/api/status", timeout=8) as response:
        status = json.load(response)
    if status.get("wifi") is not True:
        raise RuntimeError(f"ESPmote Wi-Fi is not ready: {status!r}")


def run_inquiry(
    interface: str, iac_args: tuple[str, ...], inquiry_length: int
) -> list[tuple[str, int]]:
    command = [
        "hcitool",
        "-i",
        interface,
        "inq",
        f"--length={inquiry_length}",
        "--numrsp=20",
        *iac_args,
        "--flush",
    ]
    timeout_seconds = inquiry_length * 1.28 + 2
    result = subprocess.run(
        command, check=True, capture_output=True, text=True, timeout=timeout_seconds
    )
    return parse_inquiry_results(f"{result.stdout}\n{result.stderr}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base_url", help="ESPmote URL, e.g. http://192.168.68.106")
    parser.add_argument("--interface", default="hci0", help="local Bluetooth adapter")
    parser.add_argument(
        "--mode",
        choices=("sync", "general", "limited", "compare"),
        default="sync",
        help="sync tests the normal web path; general/limited are diagnostics; compare alternates",
    )
    parser.add_argument("--cycles", type=int, default=3, help=f"bounded test cycles (1-{MAX_CYCLES})")
    args = parser.parse_args()
    if not 1 <= args.cycles <= MAX_CYCLES:
        parser.error(f"--cycles must be between 1 and {MAX_CYCLES}")
    if args.mode == "compare" and args.cycles < 2:
        parser.error("--mode compare requires at least two cycles")

    attempts: dict[tuple[str, str], int] = {}
    successes: dict[tuple[str, str], int] = {}
    for cycle in range(1, args.cycles + 1):
        if args.mode == "compare":
            discovery_name = "general" if cycle % 2 else "limited"
        else:
            discovery_name = args.mode
        endpoint, scans = DISCOVERY_TESTS[discovery_name]
        check_web_server(args.base_url)
        window_seconds = open_pairing_window(args.base_url, endpoint)
        print(
            f"Cycle {cycle}/{args.cycles}: {discovery_name} SYNC window open ({window_seconds}s)",
            flush=True,
        )
        time.sleep(2 if discovery_name == "general" else 1)

        for inquiry_name, iac_args, inquiry_length in scans:
            key = (discovery_name, inquiry_name)
            attempts[key] = attempts.get(key, 0) + 1
            devices = run_inquiry(args.interface, iac_args, inquiry_length)
            esp_found = False
            print(f"  {inquiry_name}:")
            for address, device_class in devices:
                marker = " ESPmote" if address == ESP_MOTE_ADDRESS else ""
                print(f"    {address} class=0x{device_class:06x}{marker}")
                if (
                    address == ESP_MOTE_ADDRESS
                    and device_class == EXPECTED_CLASSES[discovery_name]
                ):
                    esp_found = True
            if not devices:
                print("    no inquiry responses")
            if esp_found:
                successes[key] = successes.get(key, 0) + 1

    required_modes = ("general", "limited") if args.mode == "compare" else (args.mode,)
    failures = [
        f"{mode} {inquiry_name}: "
        f"{successes.get((mode, inquiry_name), 0)}/{attempts.get((mode, inquiry_name), 0)}"
        for mode in required_modes
        for inquiry_name, _, _ in DISCOVERY_TESTS[mode][1]
        if attempts.get((mode, inquiry_name), 0) == 0
        or successes.get((mode, inquiry_name), 0) != attempts[(mode, inquiry_name)]
    ]
    if failures:
        print("FAIL: ESPmote expected CoD was not seen consistently (" + "; ".join(failures) + ")")
        return 1

    summary = [
        f"{mode} {inquiry_name}: "
        f"{successes[(mode, inquiry_name)]}/{attempts[(mode, inquiry_name)]} "
        f"CoD=0x{EXPECTED_CLASSES[mode]:06x}"
        for mode in required_modes
        for inquiry_name, _, _ in DISCOVERY_TESTS[mode][1]
    ]
    print("PASS: " + "; ".join(summary))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

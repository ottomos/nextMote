#!/usr/bin/env python3
"""Live test of the web-controlled fake Nunchuk through a DolphinBar (mode 4).

Acts like a Wii game on the bar's hidraw slot while toggling the Nunchuk and its
inputs through the board's /api/nunchuk endpoint. Report formats follow WiiBrew
(Wiimote and Wiimote/Extension_Controllers pages):
  0x12 data reporting, 0x15 status request, 0x16 write, 0x17 read,
  0x20 status (LF bit 0x02 = extension connected), 0x21 read reply, 0x22 ack.

Dolphin must not hold the bar while this runs.
Usage: nunchuk_dolphinbar_test.py [--host 192.168.68.106] [--node /dev/hidrawN]
"""
from __future__ import annotations

import argparse
import json
import os
import select
import sys
import time
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nextmote_live_test import find_slots, probe_slot  # noqa: E402

EXT_ID = bytes([0x00, 0x00, 0xA4, 0x20, 0x00, 0x00])
DATA_REPORTS_EXT_OFFSET = {0x32: 3, 0x34: 3, 0x35: 6, 0x36: 13, 0x37: 16, 0x3D: 1}


class Bar:
    def __init__(self, node: str):
        self.fd = os.open(node, os.O_RDWR)
        self.log: list[tuple[float, bytes]] = []

    def close(self) -> None:
        os.close(self.fd)

    def send(self, packet: list[int] | bytes) -> None:
        os.write(self.fd, bytes(packet))

    def recv(self, timeout: float) -> bytes | None:
        ready, _, _ = select.select([self.fd], [], [], max(timeout, 0))
        if not ready:
            return None
        packet = os.read(self.fd, 64)
        self.log.append((time.monotonic(), packet))
        return packet

    def drain(self, seconds: float) -> list[bytes]:
        end = time.monotonic() + seconds
        packets = []
        while (packet := self.recv(end - time.monotonic())) is not None:
            packets.append(packet)
        return packets

    def wait_for(self, predicate, timeout: float) -> tuple[bytes | None, list[bytes]]:
        end = time.monotonic() + timeout
        seen = []
        while (packet := self.recv(end - time.monotonic())) is not None:
            if predicate(packet):
                return packet, seen
            seen.append(packet)
        return None, seen

    def set_mode(self, mode: int, continuous: bool = True) -> None:
        self.send([0x12, 0x04 if continuous else 0x00, mode])

    def status(self) -> bytes | None:
        self.send([0x15, 0x00])
        return self.wait_for(lambda p: p[0] == 0x20, 1.0)[0]

    def write_ext(self, address: int, data: bytes) -> int | None:
        assert 0 < len(data) <= 16
        self.send(bytes([0x16, 0x04, 0xA4, 0x00, address, len(data)]) + data.ljust(16, b"\0"))
        ack, _ = self.wait_for(lambda p: p[0] == 0x22 and p[3] == 0x16, 1.0)
        return None if ack is None else ack[4]

    def read_ext(self, address: int, size: int, middle: int = 0x00) -> tuple[int, bytes] | None:
        self.send([0x17, 0x04, 0xA4, middle, address, 0x00, size])
        data = b""
        while len(data) < size:
            reply, _ = self.wait_for(lambda p: p[0] == 0x21, 1.0)
            if reply is None:
                return None
            error = reply[3] & 0x0F
            if error:
                return error, data
            data += reply[6:6 + (reply[3] >> 4) + 1]
        return 0, data[:size]


def api(host: str, fields: dict | None = None) -> dict:
    url = f"http://{host}/api/nunchuk"
    body = None if fields is None else urllib.parse.urlencode(fields).encode()
    with urllib.request.urlopen(urllib.request.Request(url, data=body), timeout=3) as response:
        return json.load(response)


def decrypt_zero_key(data: bytes) -> bytes:
    """WiiBrew: with an all-zero key every table entry is 0x97."""
    return bytes(((b ^ 0x97) + 0x97) & 0xFF for b in data)


def ext_bytes(packet: bytes) -> bytes | None:
    offset = DATA_REPORTS_EXT_OFFSET.get(packet[0])
    return None if offset is None else packet[offset:offset + 6]


def decode(ext: bytes) -> dict:
    return {
        "sx": ext[0], "sy": ext[1],
        "ax": (ext[2] << 2) | ((ext[5] >> 2) & 3),
        "ay": (ext[3] << 2) | ((ext[5] >> 4) & 3),
        "az": (ext[4] << 2) | ((ext[5] >> 6) & 3),
        "c": not (ext[5] & 0x02), "z": not (ext[5] & 0x01),
    }


class Checks:
    def __init__(self):
        self.failed = 0

    def __call__(self, ok: bool, label: str, detail: str = "") -> bool:
        print(f"[{'PASS' if ok else 'FAIL'}] {label}{(' - ' + detail) if detail else ''}")
        self.failed += not ok
        return ok


def find_bar_slot() -> str | None:
    for _, node in find_slots():
        state, _ = probe_slot(node)
        if state == "occupied":
            return node
    return None


def wait_ext(bar: Bar, expect: dict, timeout: float = 1.5, decrypt: bool = False):
    start = time.monotonic()
    last = None

    def matches(packet: bytes) -> bool:
        nonlocal last
        ext = ext_bytes(packet)
        if ext is None:
            return False
        last = decode(decrypt_zero_key(ext) if decrypt else ext)
        return all(last[k] == v for k, v in expect.items())

    packet, _ = bar.wait_for(matches, timeout)
    return packet is not None, time.monotonic() - start, last


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="192.168.68.106")
    parser.add_argument("--node", help="DolphinBar hidraw node (default: first occupied slot)")
    parser.add_argument("--latency-samples", type=int, default=20)
    args = parser.parse_args()

    node = args.node or find_bar_slot()
    if node is None:
        print("No occupied DolphinBar slot found (is the remote connected and Dolphin closed?)")
        return 2
    print(f"Using {node}, board {args.host}")
    check = Checks()
    bar = Bar(node)
    try:
        api(args.host, {"attached": 0, "sx": 128, "sy": 128, "c": 0, "z": 0})
        bar.drain(0.3)

        # 1. Detached baseline.
        status = bar.status()
        check(status is not None and not (status[3] & 0x02), "detached: status extension bit clear",
              status.hex() if status else "no status")
        read = bar.read_ext(0xFA, 6)
        check(read is not None and read[0] != 0, "detached: 0xA400FA read errors",
              f"error={read[0] if read else None}")

        # 2. Hot-plug while streaming: unsolicited status, reporting disabled.
        bar.set_mode(0x35)
        streaming = [p for p in bar.drain(0.5) if p[0] == 0x35]
        check(len(streaming) > 10, "mode 0x35 streams before attach", f"{len(streaming)} reports/0.5 s")
        api(args.host, {"attached": 1})
        status, _ = bar.wait_for(lambda p: p[0] == 0x20, 1.5)
        check(status is not None and bool(status[3] & 0x02), "attach: unsolicited status with ext bit",
              status.hex() if status else "none")
        after = [p for p in bar.drain(0.5) if p[0] >= 0x30]
        check(len(after) == 0, "attach: data reporting stopped until host sets mode",
              f"{len(after)} data reports")

        # 3. Unencrypted init and identification (WiiBrew "new way").
        check(bar.write_ext(0xF0, b"\x55") == 0, "write 0x55 -> 0xA400F0 acked")
        check(bar.write_ext(0xFB, b"\x00") == 0, "write 0x00 -> 0xA400FB acked")
        read = bar.read_ext(0xFA, 6)
        check(read is not None and read == (0, EXT_ID), "ID at 0xA400FA is Nunchuk",
              read[1].hex() if read else "no reply")
        read = bar.read_ext(0xFA, 6, middle=0x01)
        check(read is not None and read == (0, EXT_ID), "ID mirrored at 0xA401FA")
        read = bar.read_ext(0x20, 16)
        ok = read is not None and read[0] == 0 and len(read[1]) == 16
        if ok:
            cal = read[1]
            checksum = (0x55 + sum(cal[:14])) & 0xFF
            ok = cal[14] == checksum and cal[15] == (checksum + 0x55) & 0xFF
        check(ok, "calibration at 0xA40020 with valid checksums", read[1].hex() if read else "no reply")

        # 4. Web inputs reach the HID stream.
        bar.set_mode(0x35)
        ok, _, last = wait_ext(bar, {"sx": 128, "sy": 128, "c": False, "z": False})
        check(ok and 400 <= last["az"] <= 1023, "neutral stick/buttons, gravity on Z", str(last))
        for fields, expect in (
            ({"sx": 255, "sy": 128}, {"sx": 255, "sy": 128}),
            ({"sx": 0, "sy": 30}, {"sx": 0, "sy": 30}),
            ({"sx": 128, "sy": 128, "c": 1}, {"c": True, "z": False}),
            ({"c": 0, "z": 1}, {"c": False, "z": True}),
            ({"c": 1, "z": 1}, {"c": True, "z": True}),
            ({"c": 0, "z": 0}, {"c": False, "z": False}),
        ):
            api(args.host, fields)
            ok, latency, last = wait_ext(bar, expect)
            check(ok, f"web {fields} -> HID", f"{latency * 1000:.0f} ms" if ok else str(last))

        # The DolphinBar parses 0x3D extension bytes 0/1 as Wii Remote buttons and
        # HOME (byte 1 bit 7) + a D-pad bit (byte 0 bits 0-3) switches its mode.
        # Use stick values without those bits, set before entering 0x3D.
        api(args.host, {"sx": 0xA0, "sy": 0x60})
        for mode in (0x32, 0x37, 0x3D):
            bar.set_mode(mode)
            ok, _, last = wait_ext(bar, {"sx": 0xA0, "sy": 0x60})
            check(ok, f"Nunchuk bytes present in mode 0x{mode:02x}", "" if ok else str(last))
        bar.set_mode(0x35)
        bar.drain(0.1)
        api(args.host, {"sx": 128, "sy": 128})

        bar.set_mode(0x35)
        latencies = []
        for i in range(args.latency_samples):
            value = 40 + (i * 9) % 180
            sent = time.monotonic()
            api(args.host, {"sy": value})
            ok, _, _ = wait_ext(bar, {"sy": value})
            if ok:
                latencies.append(time.monotonic() - sent)
        api(args.host, {"sy": 128})
        if latencies:
            latencies.sort()
            detail = (f"n={len(latencies)} median={latencies[len(latencies) // 2] * 1000:.0f} ms "
                      f"max={latencies[-1] * 1000:.0f} ms (includes HTTP round trip)")
        else:
            detail = "no samples"
        check(len(latencies) == args.latency_samples, "web -> HID latency samples", detail)

        # 5. Encryption with an all-zero key (WiiBrew "old way").
        check(bar.write_ext(0xF0, b"\xAA") == 0, "write 0xAA -> 0xA400F0 (enable encryption)")
        acks = [bar.write_ext(0x40, bytes(6)), bar.write_ext(0x46, bytes(6)),
                bar.write_ext(0x4C, bytes(4))]
        check(acks == [0, 0, 0], "zero key written in 6/6/4 chunks", str(acks))
        state = api(args.host)
        check(state["encrypted"] and not state["unsupported_key"], "API reports encrypted, key supported",
              json.dumps({k: state[k] for k in ("encrypted", "unsupported_key", "encryption_enables")}))
        read = bar.read_ext(0xFA, 6)
        check(read is not None and read[0] == 0 and decrypt_zero_key(read[1]) == EXT_ID,
              "encrypted ID decrypts to Nunchuk", read[1].hex() if read else "no reply")
        bar.set_mode(0x35)
        api(args.host, {"sx": 60, "z": 1})
        ok, latency, last = wait_ext(bar, {"sx": 60, "z": True}, decrypt=True)
        check(ok, "encrypted stream decrypts to web input", str(last))
        api(args.host, {"sx": 128, "z": 0})
        check(bar.write_ext(0xF0, b"\x55") == 0, "write 0x55 -> 0xA400F0 (back to plain)")
        ok, _, last = wait_ext(bar, {"sx": 128, "z": False})
        check(ok, "plain stream after disabling encryption", str(last))

        # 6. Unplug.
        api(args.host, {"attached": 0})
        status, _ = bar.wait_for(lambda p: p[0] == 0x20, 1.5)
        check(status is not None and not (status[3] & 0x02), "detach: unsolicited status, ext bit clear",
              status.hex() if status else "none")
        after = [p for p in bar.drain(0.5) if p[0] >= 0x30]
        check(len(after) == 0, "detach: data reporting stopped", f"{len(after)} data reports")
        read = bar.read_ext(0xFA, 6)
        check(read is not None and read[0] != 0, "detach: 0xA400FA read errors again",
              f"error={read[0] if read else None}")
        bar.set_mode(0x35)
        packets = [p for p in bar.drain(0.5) if p[0] == 0x35]
        check(bool(packets) and all(p[6:22] == bytes(16) for p in packets),
              "detached: extension bytes are zero", f"{len(packets)} reports")
    finally:
        try:
            api(args.host, {"attached": 0, "sx": 128, "sy": 128, "c": 0, "z": 0})
        except OSError:
            pass
        try:
            bar.set_mode(0x30, False)
        except OSError:
            pass
        bar.close()

    print(f"\n{'ALL PASS' if not check.failed else f'{check.failed} FAILED'}")
    return 1 if check.failed else 0


if __name__ == "__main__":
    sys.exit(main())

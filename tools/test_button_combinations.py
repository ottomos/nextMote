#!/usr/bin/env python3
"""Verify A+B and 1+2 button masks against expected Wii Remote core bits."""

from pathlib import Path
import re
import sys

source = Path(__file__).parents[1] / "wiimote-hid-test/main/wiimote_web.c"
text = source.read_text(encoding="utf-8")

expected = {
    "AB": 0x0008 | 0x0004,
    "ONE_TWO": 0x0002 | 0x0001,
}

for name, mask in expected.items():
    match = re.search(r'\{"' + re.escape(name) + r'",\s*(0x[0-9a-fA-F]+)\}', text)
    if not match:
        raise SystemExit(f"Missing combination {name}")
    actual = int(match.group(1), 16)
    if actual != mask:
        raise SystemExit(f"{name}: got 0x{actual:04x}, expected 0x{mask:04x}")
    print(f"{name}: 0x{actual:04x} OK")

for label in ("A + B", "1 + 2"):
    if label not in text:
        raise SystemExit(f"Missing web UI control: {label}")
    print(f"UI {label}: OK")

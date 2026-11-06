"""Shared import setup for tests that exercise the hardware utility scripts."""

from pathlib import Path
import sys

TOOLS = str(Path(__file__).resolve().parents[1] / "tools")
if TOOLS not in sys.path:
    sys.path.insert(0, TOOLS)

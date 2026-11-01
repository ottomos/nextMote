#!/usr/bin/env python3
"""Check that startup sequence comparisons do not hide unsolicited or missing reports."""

import unittest

from compare_hid_read import captured_sequence


class HidReadComparisonTests(unittest.TestCase):
    def setUp(self) -> None:
        self.core = bytes.fromhex("a1 30 00 00")
        self.reply = bytes.fromhex("a1 21 00 00 f8 17 70") + bytes(16)

    def capture(self, frames: list[bytes]) -> str:
        return "\n".join(f"{i}.000s device: RX {frame.hex(' ')}" for i, frame in enumerate(frames))

    def test_matches_all_actual_frame_bytes_in_order(self) -> None:
        frames = [self.core, self.reply]
        self.assertEqual(captured_sequence(self.capture(frames), 0x1770), frames)

    def test_unsolicited_status_is_preserved_as_a_sequence_difference(self) -> None:
        status = bytes.fromhex("a1 20 00 00 00 00 00 c0")
        expected = captured_sequence(self.capture([self.core, self.reply]), 0x1770)
        actual = captured_sequence(self.capture([status, self.core, self.reply]), 0x1770)
        self.assertNotEqual(expected, actual)

    def test_memory_error_byte_is_not_ignored(self) -> None:
        different_reply = bytearray(self.reply)
        different_reply[4] = 0xF7
        expected = captured_sequence(self.capture([self.core, self.reply]), 0x1770)
        actual = captured_sequence(self.capture([self.core, bytes(different_reply)]), 0x1770)
        self.assertNotEqual(expected, actual)

    def test_missing_or_wrong_address_response_cannot_pass(self) -> None:
        with self.assertRaises(ValueError):
            captured_sequence(self.capture([self.core]), 0x1770)
        with self.assertRaises(ValueError):
            captured_sequence(self.capture([self.core, self.reply]), 0x0016)


if __name__ == "__main__":
    unittest.main()

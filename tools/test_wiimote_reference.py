#!/usr/bin/env python3
"""Check read-only reference requests, actual reply decoding, and the capture time budget."""

import unittest

import read_wiimote_reference as reference


class ReferenceCaptureTests(unittest.TestCase):
    def test_request_reads_one_byte_without_rumble_or_memory_write(self) -> None:
        self.assertEqual(
            reference.eeprom_request(0x1770),
            bytes.fromhex("a2 17 00 00 17 70 00 01"),
        )
        with self.assertRaises(ValueError):
            reference.eeprom_request(-1)
        with self.assertRaises(ValueError):
            reference.eeprom_request(0x10000)

    def test_decode_preserves_the_actual_error_reply(self) -> None:
        frame = bytes.fromhex("a1 21 00 00 f8 17 70") + bytes(16)
        reply = reference.memory_reply(frame, 0x1770)
        self.assertIsNotNone(reply)
        if reply is None:
            self.fail("Expected a decoded memory reply")
        self.assertEqual(reply.raw, frame)
        self.assertEqual(reply.error, 8)
        self.assertEqual(reply.size_minus_one, 15)
        self.assertEqual(reply.data, bytes(16))

    def test_short_actual_error_reply_is_preserved_not_padded(self) -> None:
        frame = bytes.fromhex("a1 21 00 00 08 17 70")
        reply = reference.memory_reply(frame, 0x1770)
        self.assertIsNotNone(reply)
        if reply is None:
            self.fail("Expected an actual short memory reply")
        self.assertEqual(reply.raw, frame)
        self.assertEqual(reply.data, b"")
        self.assertEqual(reply.error, 8)

    def test_unrelated_reports_and_addresses_are_not_mistaken_for_the_reply(self) -> None:
        self.assertIsNone(reference.memory_reply(bytes.fromhex("a1 30 00 00"), 0x1770))
        self.assertIsNone(reference.memory_reply(bytes.fromhex("a1 21 00 00 08 00 16"), 0x1770))
        with self.assertRaises(ValueError):
            reference.memory_reply(bytes.fromhex("a1 21 00"), 0x1770)

    def test_capture_is_bounded_below_the_sync_window(self) -> None:
        self.assertLess(reference.CAPTURE_BUDGET_SECONDS, 20)
        self.assertEqual(reference.CONNECT_TIMEOUT_SECONDS, 6)
        self.assertEqual(reference.REPLY_TIMEOUT_SECONDS, 3)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Check native inquiry decoding and protection of the working DolphinBar session."""

import ctypes
import errno
import io
import json
import unittest
from unittest import mock

import inspect_inquiry


class InquiryInspectionTests(unittest.TestCase):
    def test_native_abi_and_endian_fields(self) -> None:
        self.assertEqual(ctypes.sizeof(inspect_inquiry.InquiryInfo), 14)
        data = bytes.fromhex("12 6e 71 b8 65 e4 01 02 03 04 25 00 34 12")
        result = inspect_inquiry.parse_inquiry_info(data)
        self.assertEqual(result.address, "e4:65:b8:71:6e:12")
        self.assertEqual(result.device_class, 0x002504)
        self.assertEqual(result.page_scan_repetition_mode, 1)
        self.assertEqual(result.page_scan_period_mode, 2)
        self.assertEqual(result.page_scan_mode, 3)
        self.assertEqual(result.clock_offset, 0x1234)

    def test_invalid_native_buffer_size_is_rejected(self) -> None:
        for length in (0, 13, 15):
            with self.subTest(length=length), self.assertRaises(ValueError):
                inspect_inquiry.parse_inquiry_info(bytes(length))

    def test_manual_measurement_fits_a_single_sync_window(self) -> None:
        inspect_inquiry.check_manual_budget(5, 2)
        with self.assertRaises(ValueError):
            inspect_inquiry.check_manual_budget(10, 2)
        with self.assertRaises(ValueError):
            inspect_inquiry.check_manual_budget(2, 12)

    def test_active_or_unknown_esp_mote_connection_is_not_disturbed(self) -> None:
        for status in ({"wifi": True, "connected": True}, {"wifi": True}):
            response = io.BytesIO(json.dumps(status).encode())
            with self.subTest(status=status):
                with mock.patch.object(inspect_inquiry.urllib.request, "urlopen", return_value=response):
                    with self.assertRaises(RuntimeError):
                        inspect_inquiry.require_idle_esp_mote("http://127.0.0.1")

    def test_idle_esp_mote_is_accepted(self) -> None:
        response = io.BytesIO(b'{"wifi":true,"connected":false}')
        with mock.patch.object(inspect_inquiry.urllib.request, "urlopen", return_value=response):
            inspect_inquiry.require_idle_esp_mote("http://127.0.0.1")

    def test_guard_runs_before_radio_inquiry_or_sync(self) -> None:
        response = io.BytesIO(b'{"wifi":true,"connected":true}')
        with (
            mock.patch("sys.argv", [
                "inspect_inquiry.py", "e4:65:b8:71:6e:12",
                "--label", "espMote", "--base-url", "http://127.0.0.1",
            ]),
            mock.patch.object(inspect_inquiry.urllib.request, "urlopen", return_value=response),
            mock.patch.object(inspect_inquiry, "run_inquiry") as inquiry,
            mock.patch.object(inspect_inquiry, "open_pairing_window") as sync,
        ):
            with self.assertRaises(RuntimeError):
                inspect_inquiry.main()
            inquiry.assert_not_called()
            sync.assert_not_called()

    def native_library(self, count: int = 1):
        result = inspect_inquiry.InquiryInfo.from_buffer_copy(
            bytes.fromhex("12 6e 71 b8 65 e4 01 00 00 04 25 00 34 12")
        )
        storage = (inspect_inquiry.InquiryInfo * 1)(result)
        library = mock.Mock()

        def inquire(device_id, length, maximum, lap, output, flags):
            self.assertEqual(device_id, 0)
            self.assertEqual(length, 2)
            self.assertEqual(maximum, inspect_inquiry.MAX_RESPONSES)
            self.assertEqual(flags, inspect_inquiry.CACHE_FLUSH)
            pointer = ctypes.cast(output, ctypes.POINTER(ctypes.POINTER(inspect_inquiry.InquiryInfo)))
            pointer[0] = ctypes.cast(storage, ctypes.POINTER(inspect_inquiry.InquiryInfo))
            return count

        library.hci_inquiry.side_effect = inquire
        return library, storage

    def test_native_giac_lap_and_owned_buffer_release(self) -> None:
        library, storage = self.native_library()
        with mock.patch.object(inspect_inquiry, "load_library", return_value=library):
            results = inspect_inquiry.run_inquiry(0, 2, False)
        self.assertEqual(len(results), 1)
        self.assertEqual(results[0].device_class, 0x002504)
        self.assertEqual(bytes(library.hci_inquiry.call_args.args[3]), b"\x33\x8b\x9e")
        library.bt_free.assert_called_once()
        self.assertEqual(library.bt_free.call_args.args[0].value, ctypes.addressof(storage))

    def test_native_liac_lap_is_not_silently_giac(self) -> None:
        library, _ = self.native_library()
        with mock.patch.object(inspect_inquiry, "load_library", return_value=library):
            inspect_inquiry.run_inquiry(0, 2, True)
        self.assertEqual(bytes(library.hci_inquiry.call_args.args[3]), b"\x00\x8b\x9e")
        library.bt_free.assert_called_once()

    def test_zero_results_still_releases_an_owned_buffer(self) -> None:
        library, _ = self.native_library(count=0)
        with mock.patch.object(inspect_inquiry, "load_library", return_value=library):
            self.assertEqual(inspect_inquiry.run_inquiry(0, 2, False), [])
        library.bt_free.assert_called_once()

    def test_invalid_native_count_is_rejected_before_buffer_access(self) -> None:
        library, _ = self.native_library(count=inspect_inquiry.MAX_RESPONSES + 1)
        with mock.patch.object(inspect_inquiry, "load_library", return_value=library):
            with self.assertRaises(RuntimeError):
                inspect_inquiry.run_inquiry(0, 2, False)
        library.bt_free.assert_called_once()

    def test_native_failure_surfaces_errno_and_releases_buffer(self) -> None:
        library, _ = self.native_library(count=-1)
        previous_errno = ctypes.get_errno()
        try:
            ctypes.set_errno(errno.EIO)
            with mock.patch.object(inspect_inquiry, "load_library", return_value=library):
                with self.assertRaises(OSError) as error:
                    inspect_inquiry.run_inquiry(0, 2, False)
            self.assertEqual(error.exception.errno, errno.EIO)
        finally:
            ctypes.set_errno(previous_errno)
        library.bt_free.assert_called_once()


if __name__ == "__main__":
    unittest.main()

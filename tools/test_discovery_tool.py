#!/usr/bin/env python3
"""Check inquiry parsing, the SYNC contract, and separate GIAC/LIAC gates."""

import contextlib
import io
import json
import unittest
from unittest import mock

import espmote_discovery_test as discovery


class DiscoveryToolTests(unittest.TestCase):
    def test_parse_inquiry_keeps_address_and_exact_cod(self) -> None:
        output = (
            "Inquiring ...\n"
            " E4:65:B8:71:6E:12 clock offset: 0x1234 class: 0x002504\n"
            " 8C:56:C5:49:6E:03 clock offset: 0x0000 class: 0x002504\n"
        )
        self.assertEqual(
            discovery.parse_inquiry_results(output),
            [(discovery.ESP_MOTE_ADDRESS, 0x002504), ("8c:56:c5:49:6e:03", 0x002504)],
        )

    def test_pairing_response_requires_the_requested_mode_and_thirty_seconds(self) -> None:
        for endpoint, mode in (
            ("/api/sync", "limited"),
            ("/api/sync/limited", "limited"),
            ("/api/sync/general", "general"),
        ):
            with self.subTest(endpoint=endpoint):
                response = io.BytesIO(
                    json.dumps({"ok": True, "window_seconds": 30, "discovery": mode}).encode()
                )
                with mock.patch.object(discovery.urllib.request, "urlopen", return_value=response):
                    self.assertEqual(
                        discovery.open_pairing_window("http://127.0.0.1", endpoint), 30
                    )
        for result in (
            {"ok": False, "window_seconds": 30, "discovery": "limited"},
            {"ok": True, "window_seconds": 12, "discovery": "limited"},
            {"ok": True, "window_seconds": 20, "discovery": "limited"},
            {"ok": True, "window_seconds": 30, "discovery": "general"},
        ):
            with self.subTest(result=result):
                response = io.BytesIO(json.dumps(result).encode())
                with mock.patch.object(discovery.urllib.request, "urlopen", return_value=response):
                    with self.assertRaises(RuntimeError):
                        discovery.open_pairing_window("http://127.0.0.1", "/api/sync")

    def run_main(self, arguments: list[str], responses: list[list[tuple[str, int]]]) -> tuple[int, str]:
        output = io.StringIO()
        with (
            mock.patch("sys.argv", ["espmote_discovery_test.py", "http://127.0.0.1", *arguments]),
            mock.patch.object(discovery, "check_web_server"),
            mock.patch.object(discovery, "open_pairing_window", return_value=30),
            mock.patch.object(discovery, "run_inquiry", side_effect=responses),
            mock.patch.object(discovery.time, "sleep"),
            contextlib.redirect_stdout(output),
        ):
            result = discovery.main()
        return result, output.getvalue()

    def test_standard_mode_requires_five_results_for_each_access_code(self) -> None:
        found = [(discovery.ESP_MOTE_ADDRESS, 0x002504)]
        result, output = self.run_main(["--cycles", "5"], [found] * 10)
        self.assertEqual(result, 0)
        self.assertIn("sync GIAC: 5/5 CoD=0x002504", output)
        self.assertIn("sync LIAC: 5/5 CoD=0x002504", output)

    def test_liac_failure_cannot_hide_behind_giac_success(self) -> None:
        found = [(discovery.ESP_MOTE_ADDRESS, 0x002504)]
        result, output = self.run_main(["--cycles", "5"], [found, []] * 5)
        self.assertEqual(result, 1)
        self.assertIn("sync LIAC: 0/5", output)
        self.assertNotIn("PASS:", output)

    def test_compare_requires_both_diagnostic_modes(self) -> None:
        general = [(discovery.ESP_MOTE_ADDRESS, 0x000504)]
        result, output = self.run_main(["--mode", "compare", "--cycles", "2"], [general, [], []])
        self.assertEqual(result, 1)
        self.assertIn("limited GIAC: 0/1", output)
        self.assertIn("limited LIAC: 0/1", output)

    def test_wrong_cod_is_a_failure_even_when_the_address_is_seen(self) -> None:
        wrong_class = [(discovery.ESP_MOTE_ADDRESS, 0x000504)]
        result, output = self.run_main(["--cycles", "1"], [wrong_class, wrong_class])
        self.assertEqual(result, 1)
        self.assertIn("sync GIAC: 0/1", output)
        self.assertIn("sync LIAC: 0/1", output)

    def test_static_giac_diagnostic_requires_explicit_opt_in(self) -> None:
        result = {"ok": True, "window_seconds": 30, "discovery": "limited",
                  "diagnostic_giac_only": True}
        response = io.BytesIO(json.dumps(result).encode())
        with mock.patch.object(discovery.urllib.request, "urlopen", return_value=response):
            with self.assertRaises(RuntimeError):
                discovery.open_pairing_window("http://127.0.0.1", "/api/sync")
        response = io.BytesIO(json.dumps(result).encode())
        with mock.patch.object(discovery.urllib.request, "urlopen", return_value=response):
            self.assertEqual(
                discovery.open_pairing_window("http://127.0.0.1", "/api/sync",
                                              allow_giac_only=True),
                30,
            )

    def test_upstream_continuous_profile_is_explicit_not_a_fake_timed_window(self) -> None:
        result = {"ok": True, "window_seconds": 0, "discovery": "general",
                  "pairing_profile": "upstream-general-continuous"}
        response = io.BytesIO(json.dumps(result).encode())
        with mock.patch.object(discovery.urllib.request, "urlopen", return_value=response):
            with self.assertRaises(RuntimeError):
                discovery.open_pairing_window("http://127.0.0.1", "/api/sync")
        response = io.BytesIO(json.dumps(result).encode())
        with mock.patch.object(discovery.urllib.request, "urlopen", return_value=response):
            self.assertEqual(
                discovery.open_pairing_window("http://127.0.0.1", "/api/sync",
                                              allow_upstream_continuous=True),
                0,
            )


if __name__ == "__main__":
    unittest.main()

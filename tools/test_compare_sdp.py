#!/usr/bin/env python3
"""Check that SDP comparisons preserve types, text, descriptor bytes, and missing fields."""

import unittest

import compare_sdp


def record(handle: str = "0x00010000", boot: str = "false", name: str = "Nintendo") -> str:
    return (
        'Searching for a service...\n<record>'
        f'<attribute id="0x0000"><uint32 value="{handle}" /></attribute>'
        '<attribute id="0x0001"><sequence><uuid value="0x1124" /></sequence></attribute>'
        f'<attribute id="0x0100"><text value="{name}" /></attribute>'
        '<attribute id="0x0202"><uint8 value="0x04" /></attribute>'
        '<attribute id="0x0206"><sequence><text encoding="hex" value="a121" /></sequence></attribute>'
        f'<attribute id="0x020e"><boolean value="{boot}" /></attribute>'
        '</record>\nService Search failed: Success\n'
    )


class SdpComparisonTests(unittest.TestCase):
    def test_dynamic_handle_is_not_an_identity_difference(self) -> None:
        left = compare_sdp.parse_record(record(), 0x1124)
        right = compare_sdp.parse_record(record(handle="0x00020000"), 0x1124)
        self.assertEqual(compare_sdp.record_differences(left, right), {})

    def test_boot_support_difference_is_not_ignored(self) -> None:
        left = compare_sdp.parse_record(record(), 0x1124)
        right = compare_sdp.parse_record(record(boot="true"), 0x1124)
        self.assertEqual(set(compare_sdp.record_differences(left, right)), {0x020e})

    def test_service_text_terminator_or_space_is_a_difference(self) -> None:
        left = compare_sdp.parse_record(record(), 0x1124)
        right = compare_sdp.parse_record(record(name="Nintendo "), 0x1124)
        self.assertEqual(set(compare_sdp.record_differences(left, right)), {0x0100})

    def test_numeric_formatting_does_not_change_the_value(self) -> None:
        left = compare_sdp.parse_record(record(), 0x1124)
        right = compare_sdp.parse_record(record().replace('value="0x04"', 'value="4"'), 0x1124)
        self.assertEqual(compare_sdp.record_differences(left, right), {})

    def test_numeric_width_remains_significant(self) -> None:
        left = compare_sdp.parse_record(record(), 0x1124)
        right = compare_sdp.parse_record(record().replace('uint8 value="0x04"', 'uint16 value="0x04"'), 0x1124)
        self.assertEqual(set(compare_sdp.record_differences(left, right)), {0x0202})

    def test_descriptor_bytes_are_compared(self) -> None:
        left = compare_sdp.parse_record(record(), 0x1124)
        right = compare_sdp.parse_record(record().replace('value="a121"', 'value="a122"'), 0x1124)
        self.assertEqual(set(compare_sdp.record_differences(left, right)), {0x0206})

    def test_missing_attribute_is_not_ignored(self) -> None:
        left = compare_sdp.parse_record(record(), 0x1124)
        right = dict(left)
        del right[0x0202]
        self.assertEqual(set(compare_sdp.record_differences(left, right)), {0x0202})

    def test_wrong_service_or_incomplete_record_is_rejected(self) -> None:
        with self.assertRaises(ValueError):
            compare_sdp.parse_record(record(), 0x1200)
        with self.assertRaises(ValueError):
            compare_sdp.parse_record(record().replace("</record>", ""), 0x1124)
        with self.assertRaises(ValueError):
            compare_sdp.parse_record(record() + record(), 0x1124)


if __name__ == "__main__":
    unittest.main()

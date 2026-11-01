#!/usr/bin/env python3
"""Compare complete wiimote/espMote SDP records without ignoring value or type differences."""

import argparse
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET


def canonical_element(element: ET.Element) -> str:
    if not isinstance(element.tag, str):
        raise ValueError("Unexpected non-data element in SDP XML")
    attributes = dict(element.attrib)
    if "value" in attributes:
        value = attributes["value"]
        if element.tag.startswith(("uint", "int")) or element.tag == "uuid":
            attributes["value"] = str(int(value, 0))
        elif element.tag == "boolean":
            if value.lower() not in ("true", "false"):
                raise ValueError(f"Invalid SDP boolean: {value!r}")
            attributes["value"] = value.lower()
        elif element.tag == "text" and attributes.get("encoding") == "hex":
            attributes["value"] = bytes.fromhex(value).hex()
    return json.dumps(
        [element.tag, sorted(attributes.items()), [canonical_element(child) for child in element]],
        separators=(",", ":"),
    )


def parse_record(output: str, service_uuid: int) -> dict[int, str]:
    matching_records = []
    for fragment in re.findall(r"<record>.*?</record>", output, re.DOTALL):
        record = ET.fromstring(fragment)
        attributes: dict[int, str] = {}
        services = []
        for attribute in record:
            if attribute.tag != "attribute" or "id" not in attribute.attrib:
                raise ValueError("Malformed SDP attribute")
            attribute_id = int(attribute.attrib["id"], 0)
            if attribute_id in attributes or len(attribute) != 1:
                raise ValueError(f"Duplicate or malformed SDP attribute 0x{attribute_id:04x}")
            attributes[attribute_id] = canonical_element(attribute[0])
            if attribute_id == 0x0001:
                services = [
                    int(uuid.attrib["value"], 0)
                    for uuid in attribute.iter("uuid")
                ]
        if service_uuid in services:
            matching_records.append(attributes)
    if len(matching_records) != 1:
        raise ValueError(
            f"Expected one complete SDP record for 0x{service_uuid:04x}, "
            f"found {len(matching_records)}"
        )
    return matching_records[0]


def record_differences(
    wiimote: dict[int, str], esp_mote: dict[int, str]
) -> dict[int, tuple[str | None, str | None]]:
    return {
        attribute: (wiimote.get(attribute), esp_mote.get(attribute))
        for attribute in sorted((wiimote.keys() | esp_mote.keys()) - {0x0000})
        if wiimote.get(attribute) != esp_mote.get(attribute)
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("wiimote", type=Path)
    parser.add_argument("esp_mote", type=Path)
    parser.add_argument("--uuid", type=lambda value: int(value, 0), required=True)
    args = parser.parse_args()
    contents = []
    for name, path in (("wiimote", args.wiimote), ("espMote", args.esp_mote)):
        output = path.read_text(encoding="utf-8")
        if "Service Search failed" in output:
            print(f"NOTE: {name} capture has a trailing search error; validating its complete record only")
        contents.append(parse_record(output, args.uuid))
    differences = record_differences(contents[0], contents[1])
    if differences:
        for attribute, (wiimote, esp_mote) in differences.items():
            print(f"0x{attribute:04x}: wiimote={wiimote}; espMote={esp_mote}")
        print(f"FAIL: {len(differences)} differing SDP attributes for 0x{args.uuid:04x}")
        return 1
    print(
        f"PASS: all {len(contents[0]) - 1} SDP attributes match for 0x{args.uuid:04x}; "
        "dynamic record handle excluded"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

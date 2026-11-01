#!/usr/bin/env python3
"""Check Wii Remote SYNC window wiring and bonded-PIN selection."""

from pathlib import Path
import re

root = Path(__file__).parents[1] / "wiimote-hid-test/main"
header = (root / "wiimote_web.h").read_text(encoding="utf-8")
web = (root / "wiimote_web.c").read_text(encoding="utf-8")
app = (root / "wiimote_app.c").read_text(encoding="utf-8")
defaults = (root.parent / "sdkconfig.defaults").read_text(encoding="utf-8")
cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")

match = re.search(r"#define WIIMOTE_SYNC_WINDOW_SECONDS (\d+)U", header)
if not match or int(match.group(1)) != 30:
    raise SystemExit("SYNC pairing window must be 30 seconds")
if "esp_timer_start_once(s_sync_pairing_timer" not in app:
    raise SystemExit("SYNC window is not time-limited")
if "WIIMOTE_SYNC_WINDOW_SECONDS * 1000000ULL" not in app:
    raise SystemExit("SYNC timer does not use the documented window length")

if "id='sync-button'" not in web or "fetch('/api/sync',{method:'POST'})" not in web:
    raise SystemExit("Web SYNC button is not wired to the pairing endpoint")
if '.uri = "/api/sync", .method = HTTP_POST, .handler = sync_limited_handler' not in web:
    raise SystemExit("Standard SYNC does not select Wii Remote limited discovery")
if '.uri = "/api/sync/limited", .method = HTTP_POST, .handler = sync_limited_handler' not in web:
    raise SystemExit("Limited-discovery diagnostic endpoint is not registered")
if '.uri = "/api/sync/general", .method = HTTP_POST, .handler = sync_general_handler' not in web:
    raise SystemExit("General-discovery diagnostic endpoint is not registered")
if "sync_window" not in web:
    raise SystemExit("Pairing-window state is not exposed to the UI")

if "param->pin_req.bda[ESP_BD_ADDR_LEN - 1 - i]" not in app:
    raise SystemExit("Bonded SYNC PIN does not reverse the host address")
if "if (param->pin_req.min_16_digit)" not in app:
    raise SystemExit("Minimum PIN length is not checked independently")
if "Using local ESP32 BT address PIN for SYNC pairing" in app:
    raise SystemExit("Minimum PIN length is still being treated as pairing mode")
if "outside the web SYNC pairing window" not in app:
    raise SystemExit("PIN requests are not limited to the explicit SYNC window")
if "CONFIG_BT_SDP_COMMON_ENABLED=y" not in defaults:
    raise SystemExit("Common SDP support is not enabled")
if "s_app.subclass = ESP_HID_CLASS_JOS;" not in app:
    raise SystemExit("HID SDP subclass does not match the captured wiimote joystick class")
if "wiimote_sdp_hid_record_is_ready()" not in app:
    raise SystemExit("SYNC does not wait for corrected HID SDP registration")
if "s_device_id_record.dip.version = WIIMOTE_DEVICE_ID_VERSION;" not in app:
    raise SystemExit("Device ID version does not use the captured wiimote policy")
if "wiimote_sdp_apply_device_id(" not in app:
    raise SystemExit("Device ID SDP policy is not applied before SYNC")
hid_open = app.split("case ESP_HIDD_OPEN_EVT:", 1)[1].split("case ESP_HIDD_CLOSE_EVT:", 1)[0]
if "wiimote_reports_send_status()" in hid_open:
    raise SystemExit("HID open still sends an unsolicited status report unlike the wiimote capture")
if "wiimote_reports_send_current()" not in hid_open:
    raise SystemExit("HID open no longer sends the captured initial core report")
if "param->close.conn_status != ESP_HIDD_CONN_STATE_DISCONNECTED" not in app:
    raise SystemExit("Discovery can be rearmed before HID disconnection is confirmed")
for value in (
    "s_device_id_record.dip.vendor = 0x057e",
    "s_device_id_record.dip.vendor_id_source = ESP_SDP_VENDOR_ID_SRC_USB",
    "s_device_id_record.dip.product = 0x0306",
    "s_device_id_record.dip.primary_record = true",
):
    if value not in app:
        raise SystemExit(f"Missing Wii Remote Device ID SDP field: {value}")

for value in (
    "set(WIIMOTE_BT_INQUIRY_SCAN_WINDOW 0x0012)",
    "set(WIIMOTE_BT_INQUIRY_SCAN_INTERVAL 0x0400)",
    "set(WIIMOTE_BT_PAGE_SCAN_WINDOW 0x0012)",
    "set(WIIMOTE_BT_PAGE_SCAN_INTERVAL 0x0800)",
    "BTM_DEFAULT_DISC_WINDOW=${WIIMOTE_BT_INQUIRY_SCAN_WINDOW}",
    "BTM_DEFAULT_DISC_INTERVAL=${WIIMOTE_BT_INQUIRY_SCAN_INTERVAL}",
    "BTM_DEFAULT_CONN_WINDOW=${WIIMOTE_BT_PAGE_SCAN_WINDOW}",
    "BTM_DEFAULT_CONN_INTERVAL=${WIIMOTE_BT_PAGE_SCAN_INTERVAL}",
):
    if value not in cmake:
        raise SystemExit(f"Missing standard inquiry / R1 page-scan configuration: {value}")
if "ESP_BT_SET_COD_SERVICE_CLASS" in app or "vTaskDelay" in app:
    raise SystemExit("The unverified delayed general-mode CoD workaround is still present")
if "esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_ALL)" not in app:
    raise SystemExit("Startup CoD does not use the verified upstream all-field initialization")
if "wiimote_discovery_set_rotation(enabled && !general_discoverable)" not in app:
    raise SystemExit("Limited SYNC does not manage capability-aware IAC rotation")
if "ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE" not in app:
    raise SystemExit("SYNC restart does not rearm inquiry before programming its initial IAC")
if "general_discoverable ? \"general-diagnostic\" : \"limited\"" not in app:
    raise SystemExit("Pairing logs do not identify the selected discovery mode")

print("Web SYNC, bonded PIN, Device ID SDP, inquiry/R1 paging, and discovery lifecycle checks passed")

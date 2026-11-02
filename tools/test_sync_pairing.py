#!/usr/bin/env python3
"""Check Wii Remote Pair, host-switch, bonded-PIN, and status wiring."""

from pathlib import Path
import re

root = Path(__file__).parents[1] / "wiimote-hid-test/main"
header = (root / "wiimote_web.h").read_text(encoding="utf-8")
web = (root / "wiimote_web.c").read_text(encoding="utf-8")
app = (root / "wiimote_app.c").read_text(encoding="utf-8")
reports = (root / "wiimote_reports.c").read_text(encoding="utf-8")
defaults = (root.parent / "sdkconfig.defaults").read_text(encoding="utf-8")
cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")

match = re.search(r"#define WIIMOTE_SYNC_WINDOW_SECONDS (\d+)U", header)
if not match or int(match.group(1)) != 30:
    raise SystemExit("SYNC pairing window must be 30 seconds")
if "esp_timer_start_once(s_sync_pairing_timer" not in app:
    raise SystemExit("SYNC window is not time-limited")
if "WIIMOTE_SYNC_WINDOW_SECONDS * 1000000ULL" not in app:
    raise SystemExit("SYNC timer does not use the documented window length")

if "<button id='sync-button' type='button' class='sync-button'>Pair</button>" not in web or \
   "fetch('/api/sync',{method:'POST'})" not in web:
    raise SystemExit("Single Pair button is not wired to the pairing endpoint")
if "syncButton.disabled=syncRequestPending||pairingPending;" not in web:
    raise SystemExit("Pair button is disabled by the connected status")
if "syncButton.textContent=left?'Pair · '+left+'s':'Pair';" not in web:
    raise SystemExit("Pair button does not display the active pairing countdown")
if "id='rearm-button'" in web or "id='rearm-explanation'" in web or "syncStatusEl" in web:
    raise SystemExit("Pairing panel still has extra controls or text")
if '.uri = "/api/sync", .method = HTTP_POST, .handler = sync_limited_handler' not in web:
    raise SystemExit("Standard SYNC does not select Wii Remote limited discovery")
if '.uri = "/api/sync/limited", .method = HTTP_POST, .handler = sync_limited_handler' not in web:
    raise SystemExit("Limited-discovery diagnostic endpoint is not registered")
if '.uri = "/api/sync/general", .method = HTTP_POST, .handler = sync_general_handler' not in web:
    raise SystemExit("General-discovery diagnostic endpoint is not registered")
if "sync_window" not in web:
    raise SystemExit("Pairing-window state is not exposed to the UI")
if "wiimote_web_set_sync_callback(request_sync_with_host_switch)" not in app:
    raise SystemExit("Pair does not route through the saved-host switch procedure")
if "return queue_virtual_cable_unplug(true, general_discoverable)" not in app:
    raise SystemExit("Pair does not release the saved virtual cable before discovery")
if "SYNC window opened after previous host virtual cable was released" not in app:
    raise SystemExit("Pair does not wait for virtual-cable release before discovery")
sync_handler = web.split("static esp_err_t sync_handler", 1)[1].split(
    "static esp_err_t sync_general_handler", 1
)[0]
if "hid_connected_snapshot()" in sync_handler:
    raise SystemExit("Pair button cannot switch hosts while HID is connected")
if "WIIMOTE_RECONNECT_INCOMING_GRACE_MS 10000U" not in app:
    raise SystemExit("Mode handoff does not allow an incoming reconnect grace period")
close_event = app.split("case ESP_HIDD_CLOSE_EVT:", 1)[1].split(
    "case ESP_HIDD_GET_REPORT_EVT:", 1
)[0]
if "s_reconnect_delay_ms = WIIMOTE_RECONNECT_INCOMING_GRACE_MS" not in close_event:
    raise SystemExit("HID close still triggers the outgoing reconnect before incoming hosts can respond")
unplug_handler = web.split("static esp_err_t virtual_cable_unplug_handler", 1)[1].split(
    "static esp_err_t sync_rearm_handler", 1
)[0]
unplug_callback = app.split("static esp_err_t request_virtual_cable_unplug", 1)[1].split(
    "\n#endif", 1
)[0]
if "hid_connected_snapshot()" in unplug_handler or \
   "if (wiimote_state_is_connected())" in unplug_callback:
    raise SystemExit("Virtual-cable unplug cannot release an active HID connection")

if "param->pin_req.bda[ESP_BD_ADDR_LEN - 1 - i]" not in app:
    raise SystemExit("Bonded SYNC PIN does not reverse the host address")
if "if (param->pin_req.min_16_digit)" not in app:
    raise SystemExit("Minimum PIN length is not checked independently")
if "Using local ESP32 BT address PIN for SYNC pairing" in app:
    raise SystemExit("Minimum PIN length is still being treated as pairing mode")
if "if (!sync_active)" not in app or \
   "is_bonded_peer(param->pin_req.bda, &bonded)" not in app or \
   "Rejecting PIN request outside SYNC from an unverified peer" not in app:
    raise SystemExit("PIN requests outside SYNC are not limited to previously bonded hosts")
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
if "WIIMOTE_SYNTHETIC_BATTERY_HALF 0x80U" not in reports or \
   "WIIMOTE_SYNTHETIC_BATTERY_HALF," not in reports:
    raise SystemExit("Status report does not advertise the synthetic half-battery level")
if "esp_err_t send_err = s_button_callback(new_state)" not in web:
    raise SystemExit("Web button requests do not surface HID report-queue failures")

print("Web Pair/host switch, bonded PIN, button reports, status battery, Device ID SDP, and discovery checks passed")

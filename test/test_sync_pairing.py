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

if "<button id='sync-button' type='button' class='pair-action sync-button'>" not in web or \
   "<span class='action-label'>Pair</span><small>next host</small>" not in web or \
   "fetch('/api/sync',{method:'POST'})" not in web:
    raise SystemExit("Single Pair button is not wired to the pairing endpoint")
if "<button id='reconnect-button' type='button' class='pair-action reconnect-button'>" not in web or \
   "<span class='action-label'>Reconnect</span><small>last host</small>" not in web:
    raise SystemExit("Reconnect button does not identify its last-host action")
if ".pair-actions{display:flex" not in web:
    raise SystemExit("Reconnect and Pair buttons are not laid out side by side")
if "fetch('/api/reconnect',{method:'POST'})" not in web or \
   '.uri = "/api/reconnect"' not in web or \
   '.handler = reconnect_handler' not in web:
    raise SystemExit("Reconnect button is not wired to its saved-host endpoint")
if "wiimote_web_set_reconnect_callback(request_saved_host_reconnect)" not in app:
    raise SystemExit("Reconnect endpoint is not wired to the saved-host reconnect callback")
reconnect_callback = app.split(
    "static esp_err_t request_saved_host_reconnect(void)", 1
)[1].split("\n}\n#endif", 1)[0]
if "request_bluetooth_stack_restart(" not in reconnect_callback or \
   "CONNECTION_OP_RECONNECT_RESTARTING_BLUEDROID" not in reconnect_callback:
    raise SystemExit("Reconnect does not restart Bluetooth while preserving the saved host")
if "esp_bt_gap_remove_bond_device" in reconnect_callback or \
   "clear_last_host()" in reconnect_callback:
    raise SystemExit("Reconnect must preserve the saved-host bond and NVS key")
if "esp_bt_hid_device_connect(target)" not in app:
    raise SystemExit("Reconnect stack restart does not reconnect to the saved host")
if "esp_bt_gap_remove_bond_device(removal_host)" not in app:
    raise SystemExit("Pair no longer removes the selected previous host bond")
if "Connecting to ESP32" in web or \
   "Loading controller status" not in web or \
   "Controller status unavailable" not in web:
    raise SystemExit("Controller status text misidentifies the nextMote web app")
if "Bluetooth HID link active" not in web:
    raise SystemExit("Connected label does not distinguish the HID link from console input acceptance")
if "Pair could not start: Bluetooth is not ready or a host release is still pending" not in web:
    raise SystemExit("Pair conflict response does not explain its pending/ready states")
if "else refreshStatus(true)" not in web or \
   "window.addEventListener('focus',()=>{if(!document.hidden)refreshStatus(true)})" not in web:
    raise SystemExit("Status does not refresh immediately when the page becomes visible")
if "if(statusRequestController&&!force)return;" not in web or \
   "new AbortController()" not in web:
    raise SystemExit("Status refreshes can overlap or remain pending indefinitely")
if "syncButton.disabled=syncRequestPending||pairingPending||reconnectRequestPending||manualReconnectActive;" not in web:
    raise SystemExit("Pair button is disabled by the connected status")
if "syncButtonLabel.textContent=left?'Pair · '+left+'s':'Pair';" not in web:
    raise SystemExit("Pair button does not display the active pairing countdown")
if "Pair · Releasing" not in web or \
   "hostReleasePending?'Releasing current host...':'Opening pairing window...'" not in web:
    raise SystemExit("Pair UI does not distinguish host release from opening discovery")
if "if(pairingPending&&s.sync_window){if(!pairingWindowStarted)startPairingCountdown()}" not in web or \
   "else if(pairingPending){const phase=pairingPhaseLabel(statusEl.dataset.pairingPhase);" not in web:
    raise SystemExit("Pair polling can overwrite the active discovery-window state")
if "default:return'Preparing pairing window'" not in web or \
   "default:return'Waiting for host release'" in web:
    raise SystemExit("Idle Pair phase is incorrectly reported as a pending host release")
if "case'restarting-bluetooth':return'Restarting Bluetooth'" not in web:
    raise SystemExit("Pair UI does not report the Bluetooth stack restart")
if "if(pairingPending&&s.connected&&s.pairing_phase==='idle'&&!s.sync_window)" not in web or \
   "'Previous host reconnected':'Connected to host'" not in web or \
   "pairingSawDisconnect" in web:
    raise SystemExit("Pair completion depends on polling a brief disconnect instead of final server state")
if "pairingWaitTimer" in web or "openDeadline" in web or \
   "Press Pair to resume" in web:
    raise SystemExit("Pair UI times out and asks for a second click while recovery is still active")
if "refreshStatus(true)}" not in web or \
   "pairingPending&&s.pairing_phase==='failed'" not in web:
    raise SystemExit("Pair UI does not keep polling until the window opens or report a real failure")
if "pairing_phase" not in web or "wiimote_web_set_pairing_phase" not in web:
    raise SystemExit("Status API does not expose the active Pair transition phase")
if "case'removing-bond':return'Forgetting current host'" not in web or \
   "statusEl.dataset.pairingPhase=s.pairing_phase||'idle';" not in web:
    raise SystemExit("Pair page does not display the live host-release phase")
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
if "last_host" not in web or "reconnecting" not in web:
    raise SystemExit("Status API does not expose the saved host and reconnect state")
if "Trying saved host" not in web:
    raise SystemExit("Status line does not identify an automatic reconnect attempt")
if "wiimote_web_set_last_host(host)" not in app or \
   "wiimote_web_set_last_host(NULL)" not in app:
    raise SystemExit("Saved-host status is not updated and cleared with reconnect state")
if "wiimote_web_set_reconnect_pending(true)" not in app or \
   "wiimote_web_set_reconnect_pending(false)" not in app:
    raise SystemExit("Status line is not synchronized with reconnect scheduling")
if "wiimote_web_set_sync_callback(request_sync_with_host_switch)" not in app:
    raise SystemExit("Pair does not route through the saved-host switch procedure")
if "CONNECTION_OP_PAIR_RESTARTING_BLUEDROID" not in app or \
   "CONNECTION_OP_RECONNECT_RESTARTING_BLUEDROID" not in app:
    raise SystemExit("Pair/Reconnect do not have explicit Bluetooth-stack restart states")
pair_request = app.split(
    "static esp_err_t request_sync_with_host_switch(bool general_discoverable)", 1
)[1].split("\n}\n#endif", 1)[0]
if "request_bluetooth_stack_restart(" not in pair_request or \
   "CONNECTION_OP_PAIR_RESTARTING_BLUEDROID" not in pair_request:
    raise SystemExit("Pair does not restart Bluetooth and clear the previous host")
restart = app.split(
    "\nstatic void bluetooth_stack_restart_task(void *arg)\n{", 1
)[1].split(
    "\n}\n\nstatic esp_err_t queue_virtual_cable_unplug", 1
)[0]
for value in (
    "release_pair_virtual_cable()",
    "esp_sdp_deinit()",
    "esp_bluedroid_disable()",
    "esp_bluedroid_enable()",
    "remove_pair_bonds(host)",
    "clear_last_host()",
    "esp_bt_hid_device_init()",
    "esp_sdp_init()",
):
    if value not in restart:
        raise SystemExit(f"Bluetooth restart is missing a Pair/Reconnect step: {value}")
if restart.index("release_pair_virtual_cable()") > restart.index("remove_pair_bonds(host)"):
    raise SystemExit("Pair removes Bluetooth bonds before releasing the stored HID virtual cable")
if "profile.plug_vc_dev_num == 0" not in app or \
   "esp_bt_hid_device_virtual_cable_unplug()" not in app:
    raise SystemExit("Pair does not release a persisted HID virtual cable before restarting Bluetooth")
vcu_release = app.split(
    "\nstatic esp_err_t release_pair_virtual_cable(void)\n{", 1
)[1].split("\n}\n\nstatic esp_err_t request_bluetooth_stack_restart", 1)[0]
if "pdMS_TO_TICKS(5000)" not in vcu_release:
    raise SystemExit("Pair VCU release does not allow the bounded completion window")
vc_unplug_handler = app.split("case ESP_HIDD_VC_UNPLUG_EVT:", 1)[1].split(
    "default:", 1
)[0]
if "s_pair_vc_unplug_pending" not in vc_unplug_handler or \
   "xSemaphoreGive(s_pair_vc_unplug_done)" not in vc_unplug_handler:
    raise SystemExit("Pair does not wait for virtual-cable-unplug completion")
resume = app.split(
    "\nstatic esp_err_t resume_connection_operation(void)\n{", 1
)[1].split(
    "\n}\n\nstatic void connection_operation_timeout", 1
)[0]
for value in (
    "CONNECTION_OP_PAIR_RESTARTING_BLUEDROID",
    "CONNECTION_OP_RECONNECT_RESTARTING_BLUEDROID",
    "CONNECTION_OP_PAIR_FAILED",
    "start_pair_window_after_release(host, general_discoverable)",
    "request_saved_host_connection(host)",
):
    if value not in resume:
        raise SystemExit(f"Bluetooth restart recovery is incomplete: {value}")
if "PAIR_TRANSITION_" in app or "s_sync_after_disconnect_pending" in app:
    raise SystemExit("Legacy Pair or SYNC-rearm state remains beside the unified state machine")
if "esp_timer_create(&connection_operation_timer_args" not in app or \
   ".callback = connection_operation_timeout" not in app:
    raise SystemExit("Connection-operation watchdog is not created and registered")
if "#define WIIMOTE_CONNECTION_OPERATION_RETRY_MS 2000U" not in app or \
   "#define WIIMOTE_CONNECTION_OPERATION_MAX_RETRY_MS 8000U" not in app:
    raise SystemExit("Connection-operation retry backoff is not bounded")
timeout = app.split(
    "\nstatic void connection_operation_timeout(void *arg)\n{", 1
)[1].split(
    "\n}\n\nstatic esp_err_t queue_virtual_cable_unplug", 1
)[0]
if "resume_connection_operation()" not in timeout or \
   "esp_timer_is_active(s_connection_operation_timer)" not in timeout:
    raise SystemExit("Watchdog expiry does not reconcile and rearm an unfinished operation")
close_event = app.split("case ESP_HIDD_CLOSE_EVT:", 1)[1].split(
    "case ESP_HIDD_GET_REPORT_EVT:", 1
)[0]
if "case CONNECTION_OP_PAIR_RESTARTING_BLUEDROID:" not in close_event or \
   "HID closed during Bluetooth stack restart" not in close_event:
    raise SystemExit("Pair does not keep its stack restart active across HID close")
nonfinal_close = close_event.split(
    "if (param->close.conn_status != ESP_HIDD_CONN_STATE_DISCONNECTED)", 1
)[1].split("wiimote_state_close()", 1)[0]
if "waiting for final close callback" not in nonfinal_close:
    raise SystemExit("Pair state is not preserved across intermediate close callbacks")
if "esp_bt_gap_remove_bond_device(removal_host)" not in app or \
   "ESP_BT_GAP_REMOVE_BOND_DEV_COMPLETE_EVT" not in app:
    raise SystemExit("Pair does not remove Bluetooth bonds before discovery")
bond_remove_handler = app.split(
    "case ESP_BT_GAP_REMOVE_BOND_DEV_COMPLETE_EVT:", 1
)[1].split("default:", 1)[0]
if "stack_bond_remove_pending_for_peer(" not in bond_remove_handler or \
   "xSemaphoreGive(s_pair_bond_remove_done)" not in bond_remove_handler:
    raise SystemExit("Bluetooth restart does not wait for each bond removal")
reconnect_callback = app.split(
    "static esp_err_t request_saved_host_reconnect(void)", 1
)[1].split("\n}\n#endif", 1)[0]
if "request_bluetooth_stack_restart(" not in reconnect_callback or \
   "CONNECTION_OP_RECONNECT_RESTARTING_BLUEDROID" not in reconnect_callback:
    raise SystemExit("Reconnect does not restart Bluetooth while preserving the saved host")
if "esp_bt_gap_remove_bond_device" in reconnect_callback or \
   "clear_last_host()" in reconnect_callback:
    raise SystemExit("Reconnect must preserve the saved host bond and reconnect target")
if "manual_reconnect" not in web or "wiimote_web_set_manual_reconnect_pending" not in app:
    raise SystemExit("Reconnect progress is not exposed to the UI")
if "SYNC_PAIRING_WINDOW_OPEN seconds=%u" not in app:
    raise SystemExit("Pair does not log the confirmed discovery window")
sync_handler = web.split("static esp_err_t sync_handler", 1)[1].split(
    "static esp_err_t sync_general_handler", 1
)[0]
if "hid_connected_snapshot()" in sync_handler:
    raise SystemExit("Pair button cannot switch hosts while HID is connected")
if "WIIMOTE_RECONNECT_INCOMING_GRACE_MS 10000U" not in app:
    raise SystemExit("Mode handoff does not allow an incoming reconnect grace period")
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

print("Web Pair/host switch, reconnect status, bonded PIN, button reports, status battery, Device ID SDP, and discovery checks passed")

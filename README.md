# nextMote

In this documentation, **wiimote** means the genuine Nintendo device and
**nextMote** means this project's implementation. Chip model names and quoted
API/log identifiers retain their original spelling.

nextMote is an ESP32 Bluetooth Classic HID-device prototype that presents a wiimote-like HID identity to a Mayflash DolphinBar. This is experimental firmware, **not a complete wiimote replacement**. Bluetooth pairing, HID channels, status/LED exchange, a Wi-Fi web controller and button reports have been exercised through the DolphinBar. I found that Mario Kart Wii accepts web-generated A input after a game restart, and that the current trace-driven build allows entry into both Mario Kart Wii and Wii Sports in the tested Dolphin + DolphinBar mode 4 setup. These are narrow, my observed results, not general compatibility certification or proof of full gameplay support.

The product and web-app name is **nextMote**. Its Bluetooth HID identity intentionally remains `Nintendo RVL-CNT-01` for Wii Remote compatibility.

> **Current milestone (6 November 2026):** The timed Limited profile uses a 30-second discovery window and CoD `0x002504`. The standard five-cycle PC test passed 5/5 GIAC and 5/5 LIAC; short 2.56-second scans measured 4/5 GIAC and 5/5 LIAC. In the user's latest host/mode sequence, Pair plus DolphinBar SYNC connected in about 1-2 seconds; Pair plus Wii U SYNC connected in Wii U mode; then Reconnect restored the link in vWii, and powering nextMote off/on led to a working vWii reconnection. After a later Wii U reset from vWii, the first nextMote input needed to re-enter vWii did not work until Reconnect was pressed; Reconnect restored operation. A direct Wii U-to-vWii transition without pressing Reconnect remains unverified. **Reconnect** restarts Bluedroid and reconnects to the saved host without changing its bond or saved address. **Pair** attempts Virtual Cable Unplug/local unplug for a plugged host, then clears stored Bluetooth bonds and `last_host`, restarts Bluedroid, and opens discovery after HID/SDP are ready; it does not reset the ESP32 or erase Wi-Fi credentials. Pair-to-laptop succeeds without an ESP reset, and a no-erase MCU reboot preserves the laptop as the sole bond instead of restoring the old DolphinBar. Three explicit saved-host Reconnect cycles passed 3/3 on the final image. An initial Reconnect stalled during BlueZ setup/authorization before eventually recovering. The user also reports that buttons and the IR camera worked inside a game; the title/mode were not specified and the IMU remains untested. Wii U Menu/game input beyond the single Reconnect-recovery event, broader game coverage, and long-duration stability remain unverified. The synthetic battery byte (`0x80`) did not produce visible battery icons in the user's Wii U test; battery reporting needs further work. D-pad, motion, and battery sensing remain unverified. See [Latest Pair and Reconnect validation](./wiimote-hid-test/README.md#latest-pair-and-reconnect-validation) for test details.

> **IMU update (`nxt12-imu-integration`, 6 November 2026):** The user has now verified IMU orientation in the web app, superseding the earlier IMU-untested status above. This branch additionally implements gravity-inclusive BNO08x acceleration in standard Wii Remote reports `0x31` and `0x33`. Host encoding/integration checks pass; the new image still needs flashing and receiver/game validation. This is not MotionPlus support. See [IMU motion integration](./wiimote-hid-test/README.md#standard-wii-remote-motion-integration).

## Hardware and software

**Integration bench follow-up (6 November):** the IMU image was flashed and initially returned fresh acceleration and a connected HID host. The user subsequently reported web status/button fetch failures while Dolphin still reported the remote connected; HTTP at the last known address also timed out. The IMU was disconnected for a requested `main` baseline comparison. Root cause and motion reception remain unverified; preserve this branch as experimental, not a confirmed gameplay pass.

- Original Bluetooth-Classic-capable ESP32 (tested on ESP32-WROOM)
- Mayflash DolphinBar in mode 4
- ESP-IDF 6.0.x with Bluedroid Classic Bluetooth HID-device support
- Linux test utilities: Python 3, PySerial, and BlueZ `hcitool` (optional for live monitoring)

ESP32-S3/C3/C6 do not provide the Classic Bluetooth controller this prototype uses.

## Build and flash

From the project directory, activate your ESP-IDF environment and build/flash using your board's serial port:

```bash
cd wiimote-hid-test
idf.py set-target esp32
idf.py build
idf.py -p PORT flash monitor
```

Replace `PORT` with the serial path for your board (for example, `/dev/ttyUSB0`). The monitor runs at 115200 baud. To exit it, press `Ctrl-]`.

nextMote advertises `Nintendo RVL-CNT-01` but starts non-discoverable. The single **Pair** button opens a 30-second Limited discovery window, shows the countdown on the button, and remains available while HID is connected. If a previous host is saved, pressing Pair releases that virtual cable and replaces the active host pairing; without pressing Pair, power-up reconnects to the last paired host. CoD `0x002504` is configured before scan enable; `/api/sync/limited` remains an alias and `/api/sync/general` remains the GIAC-only diagnostic. HCI readback shows nextMote supports only **one** inquiry access code, so it alternates LIAC/GIAC and rearms inquiry scan while retaining page scan. The latest standard five-cycle PC test passed **5/5 GIAC and 5/5 LIAC with CoD `0x002504`**; after timeout, one-cycle idle checks found nextMote in neither code. Short 2.56-second probes are less consistent (latest: GIAC 4/5, LIAC 5/5). These are discovery checks, not proof of full wiimote parity.

## Cold-boot connection test

1. Close Dolphin or other software that may claim the DolphinBar's hidraw devices.
2. Power down nextMote and unplug the bar.
3. Start the live test before plugging either device in:

   ```bash
   python3 -m pip install pyserial
   python3 test/nextmote_live_test.py
   ```

4. Plug in the DolphinBar, set it to mode 4, and power nextMote.
5. Open the Wi-Fi page at the IP printed in nextMote serial log. Press **Pair**. If a virtual cable is plugged, the firmware first requests Virtual Cable Unplug/local unplug and waits up to 5 seconds for its callback; it then removes stored Bluetooth bonds and `last_host`, restarts Bluedroid (not the ESP32), re-registers HID/SDP, and opens the 30-second discovery window. A late/missing VCU completion is logged; bond cleanup and the Bluetooth-only restart remain the fallback. Wi-Fi stays available and Wi-Fi credentials are not erased. Press the DolphinBar's physical SYNC button while the countdown is active. The user reports that this connected in about 1-2 seconds. They also report Pair plus Wii U SYNC connected in Wii U mode, followed by Reconnect in vWii and a working vWii reconnection after powering nextMote off/on. After a later Wii U reset from vWii, Reconnect was needed to restore the nextMote input required to enter vWii. See [the user-reported host/mode sequence](./wiimote-hid-test/README.md#user-reported-console-host-sequence-6-november-2026). No new serial/USB-slot capture was made for these console steps.
6. In a second terminal, monitor nextMote's occupied slot (replace `N` with the reported slot):

   ```bash
   python3 tools/dolphinbar_button_watch.py --slot N --seconds 60
   ```

   The fake-button branch sends `FAKE_BUTTON press NAME` / release logs on nextMote serial. The Wi-Fi web-control branch sends live press/release states from the page instead of cycling.

The monitor shows nextMote serial events and bar USB slot occupancy together. A successful test should show a legacy PIN request, authentication success, `WIIREMOTE_HID_CONNECTED`, a configured report mode, and an occupied DolphinBar slot. Use `Ctrl-C` to stop. Set `--port /dev/ttyUSB0` if automatic serial-port detection chooses the wrong device; use `--seconds 600` for a ten-minute observation window.

**Earlier Wii U attempts (historical):** a 20-second Limited attempt reached ACL/PIN/HID but was followed by a reported console freeze; another earlier attempt saw no connection request. Those results are superseded by the current vWii milestone above, but the freeze cause and repeatability remain unresolved. See the [pairing notes](./wiimote-hid-test/README.md#pairing-and-logs) and [latest Pair/Reconnect validation](./wiimote-hid-test/README.md#latest-pair-and-reconnect-validation).

Fresh wiimote SDP captures exposed 11 HID and five Device ID differences. The reference-matched policy now passes hardware comparison in all 24 HID and 10 Device ID attributes. The `0x1770` response matches all 23 wiimote bytes. Removing unsolicited startup status also made the actual `0x30, 0x21` sequence match exactly; explicit status requests still work. All 36 current Python tests, the ESP-IDF build, flash verification, and the final 5/5 GIAC plus 5/5 LIAC run pass. The user has now reported successful Pair connections to the DolphinBar and Wii U, Reconnect into vWii, and a working vWii reconnect after a nextMote power cycle. The user reports that in-game buttons and the IR camera worked (title/mode unspecified); the IMU remains untested. Wii U Menu input beyond the single Reconnect recovery, broader game compatibility, and long-term repeatability still need separate evidence.

Earlier controlled Wii U attempts (before the current vWii result) included a 20-second window with no incoming connection request, and an unstable attempt that froze after HID open. These are historical failure modes; the latest native-menu/vWii result is summarized above.

**DolphinBar result:** In the earlier Dolphin test, DolphinBar SYNC connected nextMote to Dolphin and the buttons worked; no serial logs or long soak were captured. In the latest host-switch test, Pair plus DolphinBar SYNC connected in about 1-2 seconds. That latest pass confirmed connection speed, not a new button/slot capture.

**Short inquiry detail:** the current 0.64-second IAC rotation and faster inquiry-scan interval preserve the observed R1 page fields. The latest 2.56-second per-code sample was GIAC 4/5 and LIAC 5/5, while the standard 7.68-second five-cycle test passed 5/5 for both. The default-off `WIIMOTE_DIAGNOSTIC_GIAC_ONLY` option remains a diagnostic only; it is not full parity.

A reviewed upstream commit explicitly claims real-Wii operation and uses continuously connectable **General** discovery while idle. My approved `WIIMOTE_UPSTREAM_DISCOVERY_BASELINE` profile now provides that lifecycle as an explicit default-off comparison: no 20-second idle timeout, hide on HID connection, rearm on confirmed disconnect. UI/API identify it as `upstream-general-continuous`; the normal timed profile is available with the flag off. Build, 36 Python contracts, and UI syntax checks pass; hardware activation requires Wii U off and DolphinBar disconnected. See the [baseline instructions](./wiimote-hid-test/README.md#upstream-discovery-baseline-profile). This is not a Wii U compatibility claim or a full reproduction of upstream BTDM firmware.

Useful additional probes:

```bash
python3 tools/dolphinbar_probe.py
python3 tools/dolphinbar_button_watch.py --slot 1 --seconds 60
python3 test/dolphinbar_sync_test.py --no-inquiry
```

The slot probe and watcher are read-only with respect to pairing. Do not run them at the same time as Dolphin: both need access to the same HID endpoints. In the fake-button demo, choose nextMote's slot with `--slot N`; run the watcher to see each press/release label.

## Current scope and limitations

- **Verified through the DolphinBar:** Classic Bluetooth pairing/authentication, HID open, host report-mode and LED commands, status request/reply, and web-generated core-button reports at the bar's USB HID slot. The captured Mario Kart Wii startup trace produced the expected error-8 reply for EEPROM probe `0x1770`, then handled report `0x1A`.
- **My game results:** Mario Kart Wii accepted web-generated A input and continued working after a game restart. After the trace-driven compatibility build was flashed, I also reported being able to enter both Mario Kart Wii and Wii Sports. These results apply to the tested Dolphin + DolphinBar mode 4 + nextMote setup only. Wii Sports Resort is future work and is not supported or tested; its MotionPlus requirements are not implemented. Full gameplay, all controls, other games, real Wii hardware, and Wii U compatibility remain unverified. Mario Kart Wii does not require a Nunchuk; the earlier Nunchuk inference was incorrect.
- The captured startup trace reads EEPROM `0x1770`; Dolphin's reference implementation rejects it with invalid-address error 8. nextMote replied `00 00 F8 17 70 00 ...`; the same trace showed output feature report `0x1A`, which nextMote now accepts and success-ACKs. No calibration read at `0x0016` appeared in that capture.
- The Wi-Fi HTTP page offers A/B/1/2, D-pad, Plus/Minus/Home and A+B/1+2. Credentials must be entered locally via `idf.py menuconfig` under **HID Example Configuration** and stay in ignored `sdkconfig`; never put them in `sdkconfig.defaults`, source, or commits.
- **Camera and pointer:** the ESP-IDF SEN0158 driver provides a live plot/table and the `wiimote-ir-pointer` branch sends camera points in report `0x33`. After the mirrored-axis change, I confirmed pointer and button operation in Wii Sports and Mario Kart. Pointer movement remains twitchy, and broader game compatibility is unverified. Startup is optional: without a camera at `0x58`, firmware logs the failure and continues Wi-Fi/Bluetooth startup. See [IR_CAMERA_STATUS.md](./IR_CAMERA_STATUS.md) and the [assembly guide](./ASSEMBLY_GUIDE.html).
- **IMU motion integration:** on `nxt12-imu-integration`, the BNO08x supplies orientation and gravity-inclusive acceleration at a requested 100 Hz. Reports `0x31` and `0x33` carry calibrated acceleration, with a 20 ms reporting tick and a neutral fallback after 250 ms without a valid sample. The web app shows the candidate HID values, source freshness and host report mode; it does not prove host receipt. Mounting axis/sign constants are in [`wiimote_accel.h`](./wiimote-hid-test/main/wiimote_accel.h). Wiring remains GPIO21/22, RST=GPIO26, INT=GPIO27. IMU-only orientation is user-verified; the new motion path has not yet been flashed or tested on a receiver.
- **Wii Sports trace fixes from `main`:** mode `0x33` uses a 17-byte buttons/accelerometer/IR payload; EEPROM `0x0000`-`0x0029` returns duplicated checksum-valid synthetic calibration; I2C writes to IR slave `0x58` are accepted and reads at `0x36` return an empty-point frame; status reflects both IR-enable stages. Mode-`0x30` button updates remain event-driven. See [the trace table](./wiimote-hid-test/README.md#wii-sports-trace-requests-prior-replies-and-corrections).
- **Motion/IR limits:** reports `0x31` and `0x33` now support BNO acceleration in source; `0x33` retains mirrored camera points when IR is enabled and available, or empty points otherwise. Camera/IMU simultaneous hardware operation and game motion behavior are not yet verified. Mounting axes must be validated before game acceptance. Pointer smoothing/calibration and MotionPlus emulation remain out of scope.
- **Not implemented:** physical controls, a real Nunchuk or other extension, smoothed/calibrated pointer behavior across games, speaker audio, or rumble. EEPROM and I2C behavior remain partial despite the tested-range calibration and IR-register shims; they are not full device emulators. Memory reads are paced in 10 ms intervals between 16-byte reports.
- **Current game scope:** I report entering Mario Kart Wii and Wii Sports in the tested Dolphin + DolphinBar mode 4 + nextMote setup, with previous A-button success in Mario Kart Wii. Wii Sports Resort is future work and is not supported or tested; its MotionPlus requirements are outside the current scope.
- **Still unverified:** D-pad behavior and game controls in vWii, motion/accelerometer reporting, pointer accuracy across the screen, stable reconnects across Wii U/vWii transitions, original Wii compatibility, and long-duration soak behavior. The observed Wii U/vWii pairing and partial input are not full game compatibility. HID connection alone does not establish full compatibility.
- **Source/library provenance:** The IMU branch depends on `rinku404/bno085` v1.2.0 through ESP-IDF Component Manager; the driver and included CEVA SH2 library are Apache-2.0. The firmware does not link or vendor WiiUse or Dolphin code. Protocol details were consulted from WiiBrew's wiimote protocol documentation, WiiUse's `wiiuse.h`/`io.c`/`wiiuse_internal.h` protocol constants and reference behavior, and Dolphin's `WiimoteConstants.h`, `WiimoteReport.h`, `WiimoteReal.cpp`, and `WiimoteEmu/EmuSubroutines.cpp`. Those repositories are protocol references only; their source files were not copied into this project. See the attribution section in [`wiimote-hid-test/README.md`](wiimote-hid-test/README.md).

## Attribution and references

I used [pknessness's upstream ESP32 Wiimote project](https://github.com/pknessness/ESPmote) as an important reference, particularly its [real-Wii initialization revision](https://github.com/pknessness/ESPmote/commit/272d810f1740585d8185a1757ad00b97be4d19b6). Thanks also to the [Loren Ashfield reference work credited upstream](https://github.com/pknessness/ESPmote/blob/272d810f1740585d8185a1757ad00b97be4d19b6/main/espmote_main.c#L204).

- Platform/drivers: [Espressif ESP-IDF/Bluedroid](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/), [rinku404/bno085](https://components.espressif.com/components/rinku404/bno085/versions/1.2.0/readme) with CEVA SH2, and [DFRobot SEN0158 documentation](https://wiki.dfrobot.com/IR_Positioning_Camera_For_Arduino_SKU_SEN0158).
- Protocol/workflow references: [WiiBrew](https://wiibrew.org/wiki/Wiimote), [WiiUse](https://github.com/wiiuse/wiiuse), [Dolphin](https://github.com/dolphin-emu/dolphin), and [XWiimote/ArchWiki](https://wiki.archlinux.org/title/XWiimote).
- Bench/pairing resources: [BlueZ](https://github.com/bluez/bluez/tree/5.72), [Nintendo's Wii U pairing instructions](https://www.nintendo.com/au/support/articles/how-to-pair-a-wii-remote-with-the-wii-u-console/), and [Mayflash DolphinBar documentation](https://www.mayflash.com/product/showproduct.php?id=6).

WiiUse and Dolphin are protocol references, not linked/copied libraries in this firmware. Existing driver/SDK licenses and provenance remain documented above and in the firmware README. The wider research overview is maintained in the workspace-level HTML plan.

See [`wiimote-hid-test/README.md`](wiimote-hid-test/README.md) for firmware details and debugging.

## Automated tests

Automated tests and interactive hardware test runners live in `firmware/test/`, separate from reusable utilities in `firmware/tools/`. From the `firmware` directory, run the automated suite:

```bash
python3 -m unittest discover -s test -v
```

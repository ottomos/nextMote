# nextMote Bluetooth Classic wiimote HID test

This ESP-IDF project configures the original ESP32 as a Bluetooth Classic HID peripheral using Bluedroid. It advertises the `Nintendo RVL-CNT-01` name, publishes Nintendo's Device ID SDP record (`057e:0306`), and implements a web-triggered, 30-second bonded SYNC window using timed Limited discovery. The current one-IAC controller profile rotates GIAC/LIAC every 0.64 seconds, with a `0x0012` inquiry-scan window at `0x0400` interval and R1 page scanning. The standard five-cycle PC test passed 5/5 for both codes with CoD `0x002504`; short 2.56-second scans were less consistent. The firmware remains a partial, experimental wiimote emulation.

The product and web-app name is **nextMote**. The Bluetooth HID name intentionally remains `Nintendo RVL-CNT-01` for Wii Remote compatibility. The ESP-IDF CMake project is named `nextMote`; the established workspace path remains `firmware/wiimote-hid-test`.

> **Current implementation status and next steps:** the latest Pair/Reconnect results and user-reported Wii U/vWii observations are in [Latest Pair and Reconnect validation](#latest-pair-and-reconnect-validation). The broader workspace experiment log is maintained outside this firmware repository.

## Previously verified with nextMote (ESP32-WROOM) + Mayflash DolphinBar (mode 4)

- Legacy PIN pairing/authentication and both HID channels open.
- The bar selects input report `0x30`, sends LED/status requests, and receives responses.
- Wi-Fi HTTP controller page loads; button POSTs update nextMote's button state.
- Web-generated core-button reports have been observed at the DolphinBar USB slot with the slot watcher.
- A cold connection was observed with serial log `WIIREMOTE_HID_CONNECTED peer=...` and DolphinBar slot status.

The DolphinBar transport and button-report path are verified. Earlier tests established A input in Mario Kart Wii and entry into Mario Kart Wii and Wii Sports. **On 6 November 2026, the user reported basic motion now seems to work in Mario Kart Wii through Dolphin + DolphinBar mode 4 on `nxt12-imu-integration`, with MotionPlus disabled.** This is standard Wii Remote accelerometer motion, not MotionPlus validation. These are narrow results; full gameplay, all controls, other games, and real Wii/vWii motion are not certified. Optional MotionPlus is implemented and off by default; Wii Sports Resort Swordplay was user-reported as an almost perfect game in Dolphin (DolphinBar, M+ build, camera + IMU).

The genuine wiimote reference reports CoD `0x002504` in GIAC/LIAC. The upstream-General comparison profile instead reports `0x000504`; the current timed Limited profile reports `0x002504`. Its standard five-cycle PC test passed 5/5 in both codes, while short 2.56-second probes remain less consistent. Laptop SDP and HID PSM comparisons are retained in the workspace-level experiment log.

The serial trace shows Mario Kart Wii reads EEPROM address `0x1770`, length 1. Dolphin's reference behavior is invalid-address error 8; nextMote returned `00 00 F8 17 70 00 ...` (metadata `0xf8`: size-minus-one 15, error 8). The host then changed report mode and requested status. The trace also shows feature report `0x1A` with ACK requested; firmware now accepts and success-ACKs it. No calibration read at `0x0016` was observed in that trace. Mario Kart Wii does not require a Nunchuk; the earlier Nunchuk inference was incorrect. After flashing, use the web **Pair** button to open the 30-second pairing window.

## Current behavior and missing wiimote features

The Wi-Fi page places **Reconnect** and **Pair** side by side: Reconnect targets the **last host**, while Pair looks for a **next host**. Both actions restart the Bluedroid host and re-register its GAP/HID/SDP state without restarting the ESP32; Wi-Fi remains available. Reconnect preserves the saved host, Bluetooth bond, and NVS reconnect key, then reconnects to that host. Pair is the host-switch action: when HIDD reports a plugged virtual cable, it requests Virtual Cable Unplug/local unplug and waits up to 5 seconds for completion before removing the ESP's stored Bluetooth bonds and `last_host`. It then restarts Bluedroid and opens Limited discovery after HID/SDP registration is ready. If VCU completion is late or missing, firmware logs the result and continues with explicit bond cleanup and the Bluetooth-only restart; it does not unregister/re-register HIDD. The Pair countdown starts only once `/api/status` confirms the window is open. If the restart is still in progress, the page reports the current `pairing_phase` and keeps polling rather than asking for a second click. The old host may retain its reciprocal pairing record; without pressing Pair, power-up still reconnects to the last successfully paired host. The 1+2 gameplay control does not trigger Bluetooth pairing. The Wi-Fi network is chosen at runtime in Wi-Fi setup mode and stored in NVS (see **Wi-Fi setup mode** below); no credentials are compiled in.

Wi-Fi/Bluetooth coexistence: the ESP32 time-shares one 2.4 GHz radio between Wi-Fi and Bluetooth Classic. The station is limited to a 20 MHz channel (set before `esp_wifi_start()`) and Wi-Fi power save is turned off once associated (`WIFI_PS_NONE`, accepted with coexistence active), so the station does not sleep through traffic for the web page. On `nxt13-fix-wifi-bt-conn` with the DolphinBar connected and the page-equivalent load (status 1 s, camera and IMU every 120 ms, a button toggle every 0.5 s), a 15-minute run had 0 status failures and 10 of about 10,600 other requests fail; the HID link never dropped, every HID send returned `ESP_OK`, and no `bcn_timeout` occurred. A following 5-minute run had 0 failures. That historical baseline kept HID acceleration neutral; the current `nxt12-imu-integration` default sends live BNO08x acceleration and includes those coexistence changes.

The status line reports the local Bluetooth HID link, saved host address, and when automatic reconnect is scheduled or underway. “Bluetooth HID link active” means nextMote's HID connection-open callback succeeded; it does not prove that Wii/vWii or a game accepted button reports. “Trying saved host” describes nextMote's retry state. The page refreshes status immediately when it becomes visible or regains focus, and avoids overlapping or hung status requests. The web `/api/button` endpoint returns an error when the firmware cannot queue the HID report. An HTTP success means the report was queued, not that the host processed it.

With Bluetooth discovery tracing enabled, `POST /api/bt/virtual-cable-unplug` remains a diagnostic request for the saved host. The regular **Pair** action also attempts VCU/local unplug when the profile reports a plugged virtual cable, then performs bond cleanup and a Bluedroid restart. Verify `/api/bt/bonds` after Pair or Reconnect; a 202 response from the diagnostic endpoint means only that the request was queued, not that the host's pairing entry was removed.

### Latest Pair and Reconnect validation

The first stack-restart-only Pair could open discovery and connect the laptop in the live Bluedroid session, but a later no-erase MCU reboot restored the old DolphinBar as `last_host` and the only bond. ESP-IDF's HIDD startup can reload a persisted `HidDeviceCabled` peer, so clearing only the currently visible bond list was insufficient. Pair now attempts VCU/local unplug before bond cleanup, then clears the stored bonds and `last_host`, restarts Bluedroid, and waits for HID/SDP readiness. The first VCU callback arrived about 150 ms after an earlier 2-second limit; the final build allows 5 seconds before the explicit bond-cleanup fallback.

On the final firmware, Pair opened a 30-second Limited window with `connected=false`, an empty `last_host`, and zero bonds. BlueZ discovered `Nintendo RVL-CNT-01` (`E4:65:B8:71:6E:12`); `bluetoothctl pair` and `connect` succeeded without an ESP reset between Pair and HID-open. A no-erase MCU reboot then preserved the laptop as `last_host` (`28:11:a8:32:4b:68`) and the sole bond; the ESP and BlueZ both returned to HID-connected, and the old DolphinBar did not return. Three explicit saved-host Reconnect cycles also passed 3/3 on the final image. PC input-event/button delivery was not tested.

Validation on ESP-IDF 6.0.1: `python3 -m unittest discover -s test -v` passes all 36 tests; `idf.py build` produces `nextMote.bin` (0x167d20 bytes, 4% app-partition space remaining). The final 460800-baud ESP-IDF flash wrote bootloader (`0x1000`), partition table (`0x8000`), and app (`0x10000`), with each image hash-verified and no NVS erase. An earlier 115200-baud attempt stopped during the app write; the final image was flashed successfully with the RAM stub.

The user has since completed a separate Wii U/vWii host sequence, recorded below. Pair intentionally clears the previous host bond when switching to a new console.

### User-reported console host sequence - 6 November 2026

1. Pressed **Pair** and DolphinBar SYNC; it connected in about 1-2 seconds.
2. Opened Wii U mode, pressed **Pair** and Wii U SYNC; it connected.
3. Entered vWii mode and pressed **Reconnect**; it reconnected.
4. Powered nextMote off and back on; it reconnected in vWii and worked.
5. While connected in vWii, reset the Wii U. After it reopened, the nextMote input the console required to enter vWii did not work initially; pressing **Reconnect** restored operation.

This user-reported sequence verifies fresh Pair connections to the DolphinBar and Wii U, saved-host Reconnect in vWii, working vWii reconnection after a nextMote power-cycle, and Reconnect recovery after a Wii U reset. The user describes the result as nearly perfect. No serial/HCI trace was captured for these console steps. Direct Wii U-to-vWii transition without pressing Reconnect, Wii U Menu/game input beyond the single recovery sequence, broader game coverage, and a long-duration soak remain separate validation gates.

### User-reported in-game input check - 6 November 2026

The user previously reported working buttons and IR camera inside a game, followed by working IMU orientation in the web app. The latest 6 November report additionally confirms basic motion seems to work in Mario Kart Wii through Dolphin + DolphinBar mode 4 with MotionPlus off. No new serial trace or detailed maneuver/axis checklist accompanied this game report.

The same page shows a live SEN0158 point plot and raw X/Y values from the native ESP-IDF I2C driver. On `wiimote-ir-pointer`, valid camera point records are passed into HID report `0x33` after the host enables IR. After the mirrored-axis build was flashed, I confirmed pointer and button operation in Wii Sports and Mario Kart. Pointer movement remains twitchy and has not been smoothed. If the camera probe fails at startup, the endpoint reports `detected:false`; see [IR_CAMERA_STATUS.md](../IR_CAMERA_STATUS.md) for the reset/SYNC diagnostic path. Camera/MPU combination is untested. See also [ASSEMBLY_GUIDE.html](../ASSEMBLY_GUIDE.html); for the guide's 5 V wiring, keep the level shifter in the SDA/SCL path.

The BNO08x uses the ESP-IDF `rinku404/bno085` component on the shared GPIO21/22 I2C bus at 400 kHz. It probes `0x4B` and `0x4A`, requests orientation at 50 Hz and gravity-inclusive acceleration at 100 Hz, and exposes diagnostics at `/api/imu`. The component requires BNO INT on GPIO27 and RST on GPIO26. The original `imu-addon` implementation displayed orientation only; `nxt12-imu-integration` adds the standard Wii Remote accelerometer report path described below. Camera-only or IMU-only startup is supported in code. Camera and IMU together were user-confirmed in Mario Kart Wii (Dolphin + DolphinBar, M+ build): steady finger cursor with live motion. Two firmware fixes were needed: the IMU service delay rounded to 0 ticks at 100 Hz (the task busy-spun on the bus lock), and the I2C interrupt now lives on core 1, away from Bluetooth/Wi-Fi, which had stretched long BNO packet reads under game traffic until camera lock waits hit the 100 ms timeout. `GET /api/bus` reports per-user lock wait/hold timing and IMU packet times (`?reset=1` returns then clears).

### IMU-only orientation check

The IMU service shares the GPIO21/22 I2C bus but starts independently of camera detection. For an IMU-only bench test, power off nextMote and connect only the GY-BNO08X: VCC/VIN to 3V3, GND to common ground, SDA/SCL to GPIO21/22, RST to GPIO26, and active-low INT to GPIO27. Leave the camera disconnected. Keep PS0/PS1 in I2C mode and AD0 at its default; firmware probes `0x4B` and `0x4A`.

Build and flash this branch, power up, and open the nextMote page. The camera panel may say “Camera not detected”; the IMU panel should show the detected address and then live roll, pitch, and yaw. `GET /api/imu` retains `detected`, `available`, `address`, `roll`, `pitch`, and `yaw`, and adds the motion diagnostics below. Rotate the board by hand to confirm the values change. The user has reported successful orientation display; that alone does not establish receiver-side motion acceptance.

### Standard Wii Remote motion integration

**Latest in-game status (6 November):** the user reports basic motion now seems to work in Mario Kart Wii through Dolphin + DolphinBar mode 4 with the default MotionPlus-off build. This supersedes the earlier game-motion-pending status. The image was built/flashed and bench-tested with game-like traffic and web load; standard mode `0x33` delivered about 50 reports/s. The user report is not a full gameplay, axis/sign, or latency certification; real Wii/vWii motion and other games remain unverified.

**Investigation and fixes:** earlier builds had web failures, in-game disconnects, and stalled IMU samples. The flashed integration image removes the explicit core split, bounds overlapping sensor HTTP polling, disables repeated rejected Bluetooth sniff requests, and drains BNO packets with a stall watchdog. The page shares one sensor-poll cycle at a time, applies timeouts through response-body parsing, skips polls while hidden, and marks acceleration unavailable on request failure. Run `node test/test_sensor_polling.js` for the polling regressions. Host/receiver and web-load tests passed, with rare HTTP timeouts and bursty receiver delivery still observed; the latest game report does not establish perfect radio timing.

On `nxt12-imu-integration`, the BNO's calibrated **accelerometer including gravity** supplies tilt and shake data. Rotation-vector angles and gravity-removed linear acceleration are not substitutes for these measurements. The default implements the base Wii Remote accelerometer. Optional MotionPlus gyroscope/extension emulation is separate and off by default; Wii Sports Resort Swordplay was user-reported as an almost perfect game in Dolphin (DolphinBar, M+ build, camera + IMU).

- [`main/wiimote_accel.h`](./main/wiimote_accel.h) contains the six mounting settings: `WIIMOTE_ACCEL_{X,Y,Z}_AXIS` select sensor axes (`0=X`, `1=Y`, `2=Z`), and `WIIMOTE_ACCEL_{X,Y,Z}_SIGN` select `+1` or `-1`. Defaults are direct X/Y/Z, pending PCB mounting checks. Rebuild after changing them; the web orientation cube remains in the sensor's original frame.
- Encoding matches the existing duplicated EEPROM calibration: zero-g is `0x80`, +1g is `0x9a` in the high-byte scale, or 512 and 616 in the 10-bit scale. Values are rounded and saturated to 0..1023. X's two low bits and Y/Z's bit 1 are packed into the unused button bits without changing real button bits. Y/Z bit 0 is not transmitted by the normal Wii Remote protocol.
- Host-selected reports `0x31` (5-byte payload) and `0x33` (17-byte payload) use live acceleration. A shared reporting task polls at 20 ms (50 Hz target); continuous mode sends each tick, otherwise only encoded changes are sent. Camera updates are cached for this same task instead of creating a second report stream. Button and command responses may additionally send immediately.
- Samples older than 250 ms, invalid samples, or an absent IMU use the previous neutral posture: zero X/Y, +1g Z. Source loss/recovery is logged. Camera absence produces empty IR points and does not prevent accelerometer reports. Mode `0x30` remains button-only and event-driven.
- `/api/imu` adds `accel_available`, `accel_samples`, `accel_age_ms` (`-1` before the first sample), `accel_ms2` (sensor frame), `accel_raw` (mapped/quantized HID candidate), `report_mode`, and `hid_connected`. The page distinguishes a motion mode requested by a host from a host that only requested buttons. Neither these diagnostics nor a successful local send proves receiver acceptance.

**Software validation:** from the firmware repository, run `sh test/run_imu_hid_test.sh`. It compiles the production encoder/report code against minimal host SDK shims, checking all 1024 raw levels, six-face gravity/scaling, swapped/inverted mounting, saturation/non-finite input, the exact 250 ms freshness boundary, buttons/status/ACK preservation, camera-present/absent report `0x33`, continuous/change-only cadence, disconnected suppression, and send-error retries. The existing Python regression suite remains `python -m unittest discover -s test -v`.

**Build:** the workspace task **nextMote: Build ESP32** uses the managed ESP-IDF 6.0.1 environment and writes the new image to the workspace's `build/imu-integration/nextMote.bin`, outside this firmware repository. This avoids the old build directory's incompatible Python environment. The dedicated **nextMote: Flash IMU integration** task targets this image; the original Flash task still targets the old default build directory. These workspace tasks build the currently checked-out source, so verify the branch before using them.

**Hardware acceptance checklist (basic Mario Kart motion is now user-confirmed; remaining checks are not implied):**

1. Flash the integration image with explicit permission, retaining NVS/bonds. With only the IMU attached, confirm `accel_available:true`, increasing `accel_samples`, and a fresh sample age.
2. With the default mapping, hold the board in six orientations: when one sensor axis points upward, expect approximately 616 on that HID axis and 512 on the other two; pointing that axis downward should give approximately 408. Verify the axis signs against the intended controller mounting. Do not drop the board to test zero-g.
3. Pair/reconnect to DolphinBar mode 4 and capture its USB HID slot while Dolphin is closed. Select a motion report using the existing host tooling and verify received `0x31` or `0x33` bytes follow the board motion, including low bits and button preservation. The bar's default button-only `0x30` mode cannot validate accelerometer delivery.
4. Close the capture before opening Dolphin with a **Real Wii Remote** connection. Test tilt/steering or shake in a base-Wii-Remote game without relying on pointer/IR. Repeat a motion-dependent action independently on Wii/Wii U vWii. Record the game, mode, mapping, and result; this is not support for MotionPlus-required games.
5. Camera+IMU together: done (Mario Kart finger cursor steady with both sensors after the bus fixes). Still repeat reconnect/power-cycle and sustained motion checks. Host tests and a build do not establish these hardware results.

### Wii Sports trace: requests, previous replies, and current behavior

The 29 October capture showed Wii Sports selecting report `0x33` and initializing the IR device at `0x58`. These are the observed request/reply corrections now present in source and flashed:

| Observed request | Previous reply | Current behavior |
|---|---|---|
| Report mode `0x33` (buttons, accelerometer, IR) | Fixed 2-byte `0x30` button report | Return the requested 17-byte payload. On `wiimote-ir-pointer`, populate its 12 IR bytes from the SEN0158 raw points when the camera frame is valid and IR is enabled; otherwise send empty points. Keep mode-`0x30` button changes event-driven. |
| EEPROM read `0x0000`, 42 bytes | Forty-two zero bytes, invalidating calibration checksums | Return duplicate synthetic IR and accelerometer calibration blocks with valid checksums. |
| EEPROM reads `0x002A` and `0x0062` | Zero-filled user-data ranges | Continue returning zero-filled data for these ranges. |
| EEPROM read `0x1770`, 1 byte | Error 8 | Keep error 8; the address is outside accessible EEPROM. |
| I2C writes to slave `0x58` and frame read at `0x36` | NACK/error 7 for writes; no frame | Store valid writes in a synthetic register bank, acknowledge them, and return a 16-byte empty-point frame. |
| IR enable reports `0x13` and `0x1A`, followed by status | Enable ACKs but IR status flag clear | Track both enable stages and set the status flag when both are enabled. |

I was able to enter Mario Kart Wii and Wii Sports with the prior flashed build. On `wiimote-ir-pointer`, I confirmed that pointer and buttons work in both games after the mirrored-axis build was flashed; pointer movement remains twitchy. The trace above describes the earlier synthetic-acceleration scaffold, now superseded by live BNO08x acceleration on `nxt12-imu-integration` and the user's basic Mario Kart motion result. These are setup-specific results, not complete gameplay or general compatibility validation. The report watcher decodes buttons from reports `0x30`-`0x33`. Optional MotionPlus is implemented; Wii Sports Resort Swordplay was user-reported as an almost perfect game in Dolphin (DolphinBar, M+ build, camera + IMU).

The firmware does **not** yet provide:

- Physical GPIO/button input.
- A real (I2C) Nunchuk, Classic Controller or other extension. A web-controlled fake Nunchuk is available (see below).
- Smoothed pointer behavior across games, speaker output, and rumble. Live accelerometer motion is implemented, with basic motion user-confirmed in Mario Kart Wii through Dolphin + DolphinBar. Mirrored-axis pointer and button operation was confirmed in Wii Sports and Mario Kart.
- Complete EEPROM/calibration and extension-I2C register behavior. The firmware returns a valid synthetic calibration map, emulates the IR register behavior seen in the trace, and optionally exposes MotionPlus registers; it is not a full EEPROM/I2C device model. Memory reads are queued and paced in 10 ms intervals between 16-byte reports.
- Full game-control coverage beyond the reported results. Original Wii compatibility, real Wii/vWii motion, MotionPlus beyond Swordplay in Dolphin, D-pad coverage, direct Wii U-to-vWii transition without Reconnect, and long-duration soak behavior remain unverified. The user reported successful vWii Reconnect, Wii U-reset recovery, and power-cycle recovery; PC saved-host Reconnect passed three cycles, but PC input-event delivery has not been tested.

Games requiring unimplemented extensions or unvalidated MotionPlus behavior may not work. The observed FIFA 15 prompt explicitly mentions the Nunchuk stick, so that title is expected to require Nunchuk-compatible extension reports; changing core button masks or sending A alone cannot satisfy that requirement. Even a button-only title may require additional wiimote initialization/report handling beyond pairing and a button report.

## Physical buttons: one Funduino joystick shield

The feature merged from `nxt16-physical-buttons` adds **six digital buttons**, matching the web controller's Up, Right, Down, Left, A and B inputs. The joystick axes and joystick click are unused; 1/2/Plus/Minus/Home remain available on the web page. Physical input is **off by default**. On 9 November 2026, the user wired shield F/E for Wii A/B, confirmed PHY A press/release feedback and reported the physical input working in Mario Kart Wii in Dolphin. This confirms that the physical input reaches the shared Wii Remote button interface. The remaining buttons, simultaneous combinations and a fuller test are deferred by the user to a later task; no full six-button or vWii validation is claimed.

The [Robotistan shield reference](https://www.robotistan.com/joystick-shield-for-arduino) lists six buttons A-F on D2-D7 and a 3.3 V/5 V selector. Match these labels against your actual board; do not use the inconsistent dual-joystick listing as a wiring reference.

### Wiring (power off first)

**Current prototype hardware:** one Funduino shield, with its 3.3 V/5 V lever set to **3.3 V**. This is the temporary cabling plan until the custom button hardware is designed; it is not a final custom-PCB pinout.

Use jumper wires, not the Arduino shield stacking headers directly on the ESP32. Set the shield selector to **3.3 V**, use a common GND and supply the appropriate shield power input from ESP32 3V3. Confirm that the shield's button signals stay within 0-3.3 V before attaching them to ESP32 inputs. **Never connect a 5 V button signal to the ESP32.** Leave any 5 V rail unconnected.

| Shield button / signal | Wii/web control | ESP32 GPIO |
|---|---|---|
| A / D2 (top direction) | Up | 32 |
| B / D3 (right direction) | Right | 33 |
| C / D4 (bottom direction) | Down | 25 |
| D / D5 (left direction) | Left | 14 |
| F / D7 (small button) | A | 23 |
| E / D6 (small button) | B | 19 |

Shield D2 is **not** ESP32 GPIO2. Verify the actual top/right/bottom/left button labels; if your revision differs, adjust the signal wires to preserve the direction-to-control mapping. Leave AN0, AN1 and D8/KEY unconnected. These six ESP32 inputs use internal pull-ups: released is HIGH and pressed is LOW. Existing sensor and USB serial wiring stays unchanged.

### Enable and test

With ESP-IDF activated, build from this project directory:

```bash
idf.py -B build/mplus -DNEXTMOTE_ENABLE_MOTION_PLUS=1 -DNEXTMOTE_ENABLE_PHYSICAL_BUTTONS=1 build
```

The physical-button flag is independent of MotionPlus. After wiring is verified, flash that build using the usual serial port. CMake remembers the flag in the build directory; explicitly set `-DNEXTMOTE_ENABLE_PHYSICAL_BUTTONS=0` and rebuild to disable it.

Inputs are sampled about every 10 ms and must stay stable for 30 ms to change state. Simultaneous buttons are supported. Physical and virtual masks are OR-combined: releasing a web button cannot release the same button while physically held, or vice versa. Physical input remains available without Wi-Fi; setup mode does not start the GPIO task. The automatic fake-button demo must be disabled when this feature is enabled.

First bench test: check the startup line `Shield buttons enabled: Up=32 Right=33 Down=25 Left=14 A=23 B=19`, then press/release every button while watching `Physical mask=...` serial messages. Expected individual masks: Up `0x0800`, Right `0x0200`, Down `0x0400`, Left `0x0100`, A `0x0008`, B `0x0004`; released `0x0000`. Then verify these inputs through the host/game, simultaneous A+B and a direction+A, and mixed physical/web holds. A serial mask alone is not proof of host receipt. The web button highlighting is not a physical-input monitor.

The web app has a separate physical-input line: **PHY Pressed A**, **PHY Released A**, etc., independent of Bluetooth connection. It polls status about once per second; transition counters let it show both press and release if a tap happens between polls. This is a diagnostic summary, not a timestamped event log: repeated transitions may be grouped and ordering across buttons is not preserved. The last summary stays visible. Web-click feedback remains separate and physical presses do not highlight the virtual buttons.

Physical and web controls feed the **same Wii Remote button state and HID report interface**; there is no separate game-facing physical-button protocol. The Mario Kart Wii result confirms the physical-input path in the user's tested Dolphin setup. Platform labels in the test records describe where testing occurred, not different button implementations.

Validation: host tests cover debounce and release bounce, timer wraparound, all 64 combinations, mixed-source holds/releases and reconnect state. ESP-IDF 6.0.1 builds with physical buttons enabled. The first hardware flash lost communication twice; with the shield disconnected, a complete 115200-baud flash succeeded and verified its hash. After reconnection, the user confirmed PHY A press/release and a Mario Kart Wii result in Dolphin. This is a limited user-reported result, not full six-button or real-console certification.

## Wi-Fi setup mode

On a factory-fresh board (no network stored in NVS) nextMote starts in **Wi-Fi setup mode**. In this mode only the setup network and page run: Bluetooth, the IMU and the camera are **not** started.

1. Power the flashed board. Join the open Wi-Fi network `nextMote-Setup-XXXX` (last four hex digits of the SoftAP MAC). Stay connected if your phone warns that this network has no internet. The captive DNS and redirect may open the page automatically; otherwise open `http://192.168.4.1/`.
2. Pick your network from the scan list, enter the password and press connect. **ESP32-WROOM supports only 2.4 GHz.** A shared 2.4/5 GHz network name is fine: the ESP joins its 2.4 GHz radio, not its 5 GHz radio.
3. Wait for the connection result. Errors (wrong password, network not found) are shown on the page; correct the details and retry. On success the device stores the network in NVS, displays its assigned IP and restarts into normal mode after 10 s.
4. Reconnect your phone/computer to the home network and open `http://<assigned-IP>/`. The setup AP goes away; Bluetooth and sensors start in normal mode. The setup page does not automatically move your phone onto the home network.

### Subsequent boots and finding the web app

With a network saved, each boot connects directly to it; no setup AP is started. The router assigns the normal-mode IP using DHCP, so **it can change after a restart**. Do not treat an address from an earlier test as permanent. The setup-mode address remains `192.168.4.1`.

Read the startup line `wiimote_web: Wi-Fi connected; IP=...` in the serial monitor. From the project directory, use `idf.py -B build/mplus -p /dev/ttyUSB0 monitor` for the tested M+ build (replace the build directory/port as needed; ESP-IDF must be activated). Exit with `Ctrl+]`. Opening the monitor may reset the board and interrupt Bluetooth. `/api/status` also returns `ip` and `ssid` once you already know the address. The serial `status` command shows mode and stored network name, not the IP.

For a stable address, configure a DHCP reservation in your router. A `nextmote.local` hostname is not implemented.

### Re-entering setup and factory Wi-Fi reset

Triggers for testing (the stored network is kept unless you use `forget`):

- Web app: **Wi-Fi setup** button (`POST /api/wifi/setup`).
- Serial console (115200 baud, type a command and press Enter): `status`, `setup` (reboot into setup mode once), `normal` (leave setup mode when a network is saved), `forget` (erase the stored network = factory Wi-Fi state), `reboot`.
- The setup request is one-shot: resetting the board while in setup mode returns to normal mode when a network is stored.

`forget` clears only the Wi-Fi configuration namespace, not Bluetooth bonds or the saved Bluetooth host. Ordinary reflashing does not clear these settings. Full flash/NVS erasure is different and can remove pairing data too.

There is no automatic fallback to setup mode when the stored network is unreachable: Bluetooth operation remains available, but the normal web page may not be reachable. Use serial `setup` to choose a new network. No physical setup-button trigger is implemented.

The setup AP is open and the setup page uses HTTP, not HTTPS. Configure the device in a trusted environment; do not publish passwords in screenshots, logs or repository files.

### Initialization validation (7 November 2026)

The user reports provisioning **"works really well"**. Instrumented laptop tests separately verified factory boot, page loading, captive redirect/DNS, scanning, wrong-password failure followed by successful retry, credential persistence, return to the normal web app and DolphinBar reconnection. Serial `setup`, `normal`, `forget`, web-triggered setup and one-shot reset behavior also passed. The user's exact browser/device and whether its automatic captive-portal popup opened were not specified. Different-channel routers and open home networks remain untested.

Setup-mode API: `GET /api/scan`, `POST /api/wifi` (form fields `ssid`, `password`), `GET /api/wifi/state`, `POST /api/setup/exit`.

## Build and flash

Requires ESP-IDF 6.0.x. Use an ESP32 target and the serial port for your board:

```bash
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

The custom `partitions.csv` gives the app almost the whole 4 MB flash (factory app 0x3F0000 at 0x10000; NVS at 0x9000 is unchanged, so Bluetooth bonds and Wi-Fi settings survive a reflash). After changing the partition table do a full `idf.py flash`.

The project defaults in `sdkconfig.defaults` enable Bluetooth Classic HID-device mode, set the advertised name to `Nintendo RVL-CNT-01`, and increase the SDP attribute/padding buffers to publish the descriptor. The standard web SYNC endpoint opens a 30-second Limited window with CoD `0x002504`. `/api/sync/limited` is an alias of the standard path; `/api/sync/general` is the GIAC-only diagnostic.

### Quick iteration

- Work from `firmware/wiimote-hid-test` in the nextMote repository, not the separate legacy `espWiimote` Arduino project.
- For normal source edits, use incremental `idf.py build`; reserve `idf.py fullclean` for stale build/configuration problems.
- On the tested ESP32, flash at 115200 baud: `idf.py -p /dev/ttyUSB0 -b 115200 flash`. If esptool reports that the stub failed to start, no image was written; resync with `python -m esptool --chip esp32 -p /dev/ttyUSB0 -b 57600 --no-stub read-mac`, then retry the flash.
- A flash resets the HID connection; press DolphinBar SYNC again. Verify `/api/status` and `/api/camera` at the IP printed by the firmware. The current camera initialization uses the DFRobot default sensitivity profile.

## Pairing and logs

**Current Wii U/vWii result (1 November 2026):** with console SYNC first and web SYNC as the remote-side step, HCI recorded connection completion, reverse-host-address PIN success, authentication success, and HID open. In Wii U Menu I pressed Plus on the web controller and opened the Wii application. In vWii I confirmed Plus and Minus page changes and saw the pointer after repositioning the camera/sensor-bar setup. This is a real connection and partial-input result, not full compatibility. D-pad, motion, game-specific controls, and longer stability/reconnect remain unverified. Older freeze/no-request attempts remain historical; their cause is unknown.

The DolphinBar has two separate roles in these tests. I used it only as an IR sensor bar during the vWii pointer test. In a separate test, I pressed DolphinBar SYNC, connected nextMote to Dolphin, and confirmed the buttons worked. I did not capture serial instrumentation for that bar test.

I verified a wiimote connects to Wii U slot 1 with the same console procedure. The absence of nextMote PIN/authentication/HID logs does not show that the console transmitted nothing: incoming radio inquiries are handled inside nextMote's Bluetooth subsystem and are not reported as normal GAP events.

For diagnostics, enable **Trace Bluetooth discovery HCI commands and connection events** under **HID Example Configuration** in local `menuconfig`. This opt-in trace uses project-local linker wrappers for the ESP-IDF 6 Bluedroid transport, forwards packets unchanged, and logs selected HCI TX/RX packets with timestamps. It captures nextMote HCI completion status, IAC/CoD/scan writes, and connection requests. PIN values, link keys, and ACL payloads are excluded; dropped or malformed records are reported explicitly. The trace is off by default, does not alter global ESP-IDF sources, and cannot sniff radio inquiry traffic. Native sanitizer tests, firmware build, flash hash verification, and live HCI TX/RX capture passed.

nextMote Bluetooth reads report one supported IAC. The previous Limited request submitted LIAC/GIAC together, received success, but read back only LIAC. The previous General path wrote CoD after scan enable and still advertised `0x000504` in the last laptop measurement. nextMote Bluetooth command acceptance and cached class readback therefore did not prove on-air parity.

The standard `/api/sync` selects Limited mode; `/api/sync/limited` remains an alias and `/api/sync/general` is the explicit GIAC-only diagnostic. Bluedroid sets the Limited CoD before enabling scans. At boot, a project-local ESP-IDF 6 adapter reads nextMote's IAC capacity; for a one-IAC nextMote it submits one code at a time, alternates LIAC/GIAC every 1.28 seconds, and rearms inquiry scan around each update. nextMote hardware supporting two codes retains the original two-code request. Rotation stops outside discovery and on failure; PIN policy is unchanged. The adapter changes IAC configuration at the stack-helper boundary, while the tracer forwards transport packets unchanged. Native/Python tests, trace-enabled and trace-disabled builds, flash hash verification, and the measured discovery gate pass.

With the trace enabled, `GET /api/bt/controller` queues read-only HCI requests for CoD, current/supported IAC count, and scan enable/activity/type. Its JSON response explicitly means **queued**, not nextMote Bluetooth command acceptance or parity. Results appear in `BT_HCI_RX` serial records. The endpoint passed build/flash and live readback checks. Requests use Bluedroid's credit-managed queue and ESP-IDF 6 private headers, not raw VHCI writes, and do not expose credentials.

The current timed profile uses inquiry window/interval `0x0012`/`0x0400` and page window/interval `0x0012`/`0x0800`, preserving the measured R1 paging response. It alternates one supported inquiry access code every 0.64 seconds and is hidden outside the 30-second window. To repeat the local discovery comparison (requires `hcitool` and a laptop BR/EDR adapter):

```bash
python3 test/nextmote_discovery_test.py http://nextMote-IP --mode sync --cycles 5
```

The bounded standard test opens a fresh 30-second SYNC window per cycle and checks both GIAC and LIAC. Five cycles pass only if nextMote appears with CoD `0x002504` in all five scans of each code. `--mode general` tests the GIAC-only diagnostic with expected CoD `0x000504`; `--mode limited` tests the Limited alias. `--mode compare` requires both diagnostic modes to pass and at least two cycles. These tests measure discovery only; the one-code adapter does not claim timing identical to a wiimote.

The first rotating implementation passed native/Python/build/flash checks but returned **GIAC 0/5 and LIAC 0/5**. Its IAC writes received status `0x00`; the static General path still returned nextMote with CoD `0x000504` in 1/1 scan. Rearming inquiry around each IAC update then passed **GIAC 5/5 and LIAC 5/5, both CoD `0x002504`**. After timeout, nextMote answered neither code and HCI readback reported scan enable `0x00`. Native tests, six Python contract/threshold tests, build and flash hash verification pass. This is a discovery result, not a freeze fix or full wiimote emulation.

The initial nextMote HID record advertised subclass `0x08` and boot-device support, unlike the captured wiimote. The corrected policy now reads back identically in all 24 HID and all 10 Device ID attributes. Not all report modes or full game behavior are implemented, so exact SDP matching is not full compatibility.

The wiimote reference is now captured: the previous nextMote image differs in 11 HID attributes and five Device ID attributes, while the report descriptor matches byte-for-byte. The new SDP policy selects subclass `0x04`, country `0x33`, virtual-cable/boot/normally-connectable false, remote-wake true, SDP-disable false, supervision timeout `0x0c80`, and exact non-NUL-terminated service text. Device ID uses specification `0x0100`, captured device version `0x8500`, and the wiimote protocol/browse/profile lists. These are measured reference values, not guesses about generic HID defaults.

`wiimote_sdp.c` applies this policy with existing ESP-IDF SDP helpers and a project-local HID-builder wrapper, without replacing the HID backend or editing global SDK files. HID/Device ID readiness gates SYNC; failures log and clean up incomplete registration. Native values, eight SDP-comparison tests, discovery checks, build, flash hash verification and exact hardware SDP readback pass.

The prepared EEPROM capture finished in 2.342 seconds: the wiimote and nextMote `0x1770` error replies match all 23 bytes (`0xF8`, error 8). The previous nextMote sent unsolicited startup `0x20` before core `0x30`; that send is removed. The flashed image now matches the complete captured sequence `0x30, 0x21` byte-for-byte. An explicit `0x15` request still receives the six-byte `0x20` status report. Its last byte is a synthetic midpoint, `0x80` on the protocol's 0–255 scale; the user reports that this did not make the Wii U show battery icons. There is no battery-voltage measurement yet. The user also reports that a Wii U Menu pairing, followed by entering vWii and power-cycling nextMote, restored vWii input without another Pair press. The direct transition without an ESP restart remains the open test; a 10-second incoming-host grace before outgoing retry is in the firmware.

The subsequent single controlled Wii U attempt did not connect: no incoming HCI connection request, authentication, or HID opening occurred before the 20-second timeout. I confirmed the console stayed responsive on its pairing instructions. Further unchanged console retries are paused; investigate below the already-verified SDP/read-response layer.

The new `tools/inspect_inquiry.py` reads paging fields omitted by ordinary `hcitool` output. A prepared short capture measured wiimote GIAC 5/5 at 2.56 seconds and repetition R1; idle nextMote was 0/5 at that duration but visible in a longer inquiry with repetition R0. The next test restores the default page window to target R1, without also changing IAC phase. Run only when Wii U is off and nextMote is disconnected; automatic SYNC is explicitly refused while a HID host is using it.

With discovery tracing enabled, the prepared `/api/bt/bonds` endpoint returns bonded-peer addresses/count only. It checks Bluetooth readiness and snapshot stability, logs failures, and neither exposes keys nor removes bonds. It is not present in the previously deployed image until the diagnostic build is safely flashed.

The read-only bond endpoint is now deployed and reports the two known hosts, DolphinBar and Wii U, without exposing or deleting keys. R1 paging now matches the wiimote, but short rotating GIAC results remain below its 5/5 baseline. The current bounded `WIIMOTE_DIAGNOSTIC_GIAC_ONLY` build option holds GIAC with Limited CoD to isolate timing; it defaults off and is marked in the SYNC JSON response. `inspect_inquiry.py` explicitly permits this diagnostic, while the full parity test rejects it. No held-code result should be described as both-code parity.

Normal-button bonded reconnection remains a separate implementation gap: the wiimote actively tries saved hosts after a normal button press, whereas red SYNC stays passive. The SDK bond-list order is not documented connection recency, so a reconnect destination must not be guessed from its first entry.

### Upstream-discovery baseline profile

The verified upstream real-Wii commit is [272d810](https://github.com/pknessness/ESPmote/commit/272d810f1740585d8185a1757ad00b97be4d19b6). Its enabled path uses General discovery at registration and after confirmed disconnect, hides on HID opening, and uses reversed-peer six-byte legacy PINs. These source facts differ from our timed Limited lifecycle; the real-Wii claim is not a Wii U verification.

For my approved lifecycle comparison, build with `-D WIIMOTE_UPSTREAM_DISCOVERY_BASELINE=ON -D WIIMOTE_DIAGNOSTIC_GIAC_ONLY=OFF`. This explicit default-off profile automatically arms connectable General discovery once both SDP records are ready, hides during HID connection, and rearms on confirmed disconnect. Keep Wii U off and DolphinBar disconnected until the recorder is ready. **There is no 20-second timeout while idle.** The UI labels the profile; JSON reports `pairing_profile: upstream-general-continuous` and `window_seconds: 0`.

The normal timed profile is restored with `-D WIIMOTE_UPSTREAM_DISCOVERY_BASELINE=OFF`. Timed/parity tooling rejects the continuous profile by default; the single-code inquiry inspector explicitly opts in. Verified SDP/read behavior and Classic-only transport remain unchanged, so this is not a byte-for-byte reproduction of the full BTDM upstream firmware. The unused upstream 16-zero-PIN fallback is intentionally not copied.

Each new HID session now resets report mode to core `0x30` and clears the previous host's continuous-report flag. A native regression failed before that fix and passes after; the ESP-IDF build passes. This prevents a prior `0x33` session from contaminating initial data, but is not claimed to explain a failure before ACL connection.

To compare two captured records, run `python3 tools/compare_sdp.py WIIMOTE_XML nextMote_XML --uuid 0x1124` for HID or `--uuid 0x1200` for Device ID. It checks all values and types, including descriptor bytes and service text; only the dynamic handle is excluded. Trailing `sdptool` search errors are reported separately from complete records.

Expected serial evidence includes:

```text
READY identity=Nintendo RVL-CNT-01; use web SYNC to open a 30-second pairing window
Nintendo Device ID SDP record ready: handle=...
Controller supported inquiry access codes=1
SYNC_PAIRING_WINDOW_OPEN seconds=30 pairing=bonded discovery=limited inquiry_window=0x0012 inquiry_interval=0x0400 page_window=0x0012 page_interval=0x0800
SYNC inquiry code=GIAC count=1
SYNC inquiry code=LIAC count=1
legacy PIN request min16=0
legacy PIN reply result=ESP_OK mode=SYNC pin=reverse-host-address
authentication status=0 peer=<host>
WIIREMOTE_HID_CONNECTED peer=...
report mode=0x30
LED state=...
```

For SYNC bonding, the legacy PIN is the host's Bluetooth address in reverse byte order. `min_16_digit` is only a minimum-PIN-length flag, not the pairing-mode selector. Requests requiring 16 digits are rejected because the wiimote path uses a six-byte address PIN. Standard Limited SYNC targets CoD `0x002504`; the General diagnostic uses `0x000504`. The previous after-scan CoD override and arbitrary waits have been removed. On-air acceptance of the new adapter and stable Wii U behavior remain unverified.

wiimotes also have a separate temporary 1+2 guest-pairing mode with different PIN behavior. That non-persistent guest mode is not implemented here; Bluedroid's successful Classic authentication stores link keys. The web page's 1+2 button is gameplay input only.

## Protocol research and acknowledgements

The firmware protocol behavior was implemented in our own code after consulting:

- [WiiBrew: wiimote](https://wiibrew.org/wiki/Wiimote) for community-documented reports, memory spaces and extension initialization.
- [WiiUse](https://github.com/wiiuse/wiiuse), especially `src/wiiuse.h`, `src/wiiuse_internal.h`, and `src/io.c`, for button masks, report identifiers, memory commands and the common initialization handshake. WiiUse is a GPL-3.0-or-later project; no WiiUse library or source files are linked/copied into this firmware.
- [Dolphin Emulator](https://github.com/dolphin-emu/dolphin), especially `Source/Core/Core/HW/WiimoteCommon/WiimoteConstants.h`, `WiimoteReport.h`, `Source/Core/Core/HW/WiimoteReal/WiimoteReal.cpp`, and `Source/Core/Core/HW/WiimoteEmu/EmuSubroutines.cpp`, for packet structure, error codes and memory read/write behavior. Dolphin is GPL-2.0-or-later; no Dolphin source files are linked/copied into this firmware.

This is protocol-reference attribution, not a claim of code reuse. Any future copying/adaptation of source must be separately reviewed for license obligations and identified in the relevant files.

The IMU branch obtains BNO08x support from `rinku404/bno085` v1.2.0 through ESP-IDF Component Manager. The component and its included CEVA SH2 library are Apache-2.0; the dependency version is pinned in `dependencies.lock`.

## Implementation notes

- `main/wiimote_app.c` contains Classic Bluetooth setup, pairing, GAP/HID event callbacks, and app startup.
- `main/wiimote_state.c` owns synchronized connection, button, LED, IR, and report-mode state.
- `main/wiimote_protocol.c` defines supported report IDs and common protocol checks.
- `main/wiimote_reports.c` encodes and sends input, status, acknowledgement, and memory replies, and owns the paced sensor-report task.
- `main/wiimote_accel.c` maps mounting axes and encodes calibrated acceleration including the packed button-byte low bits.
- `main/wiimote_commands.c` dispatches host output reports to their capability handlers.
- `main/wiimote_memory.c` emulates EEPROM/I2C reads and writes for initialization.
- `main/wiimote_controls.c` handles web button input and the optional fake-button demo.
- `main/ir_camera_service.c` initializes and polls the SEN0158 camera for the web preview.
- `main/sensor_i2c_bus.c` creates and serializes access to the shared camera/IMU I2C bus.
- `main/imu_service.c` initializes the BNO08x and publishes rotation-vector orientation and timestamped gravity-inclusive acceleration.
- `main/wiimote_descriptor.c` contains the HID report descriptor.
- Motion reports and sniff mode: Bluedroid's default link policy allows sniff, so after every HID report it asked the DolphinBar for sniff mode, which the bar always rejects (`0x1a Unsupported Remote Feature`). With the 50 Hz IMU stream this HCI churn and its serial logging starved the Bluetooth task: the host stopped getting status/memory/ack replies within seconds and Dolphin reported the Wii Remote disconnected. The build now compiles Bluedroid with `BTA_DM_LINK_POLICY_SETTINGS=0x000B` (role switch, hold, park; no sniff) and only logs failed `ESP_HIDD_SEND_REPORT_EVT`s. `NEXTMOTE_SEND_IMU_TO_HID` (default 1, or `-DNEXTMOTE_SEND_IMU_TO_HID=0`) keeps the IMU web-only with neutral HID acceleration for A/B testing. `tools/dolphinbar_game_emu.py /dev/hidrawN 30` emulates a game's WPAD traffic through the DolphinBar (Dolphin closed) and reports missed replies and data rate.
- Emulated Wii MotionPlus (`main/wiimote_motion_plus.c`) is **off by default**. To try it, use a separate build dir: `idf.py -B build/mplus -DNEXTMOTE_ENABLE_MOTION_PLUS=1 build flash`. It implements the inactive (0xA600FA) and active (0xA400FA) IDs, activation/deactivation writes and challenge registers, and fills the 6 extension bytes of modes 0x32/0x34/0x35/0x36/0x37/0x3d from the BNO08x calibrated gyro (100 Hz, accelerometer axis mapping). The sensor stream runs at 100 Hz while M+ is active. Stress-tested with the game emulator (mode 0x37 + web load, 5 min, 0 send failures); gyro axis/sign is not yet verified in a game. `/api/imu` also reports `gyro_samples`, `motion_plus_active`, `hid_data_sent` and `hid_send_failures`. While HID is connected the firmware sets the coexistence "A2DP streaming" status bit so Wi-Fi gives the HID stream enough airtime (this removed `HID_ERR_CONGESTED` drops at 100 Hz).
- Fake Nunchuk (`main/wiimote_nunchuk.c`, branch `nxt14-fake-nunchuk`): the web page's Nunchuk panel has a Connect/Disconnect toggle (default disconnected, because some games refuse an extension), a virtual stick and C/Z buttons. `GET /api/nunchuk` returns state; `POST /api/nunchuk` takes form fields `attached=0|1`, `sx`/`sy` (0-255), `c=0|1`, `z=0|1`. When attached it answers extension slave `0x52` with the Nunchuk ID `00 00 A4 20 00 00`, calibration at `0x20` (mirrored at `0x30`, checksum-valid), and 6 data bytes in modes 0x32/0x34/0x35/0x36/0x37/0x3d (accelerometer held neutral, gravity on Z). Toggling sends an unsolicited status report and disables data reporting until the host sets a mode again, like a real plug event. Encryption (0xAA to 0xF0) is emulated for the all-zero key only; a non-zero key is flagged as `unsupported_key` in the API and data stays plain. With the M+ build, active MotionPlus owns `0x52`; while the Nunchuk is attached the M+ frames set byte 4 bit 0 (extension connected), and Nunchuk passthrough mode (`0x05` written to `0xA600FE`, ID `00 00 A4 20 05 05`) interleaves M+ frames with WiiBrew-format passthrough Nunchuk frames (byte 5 bit 1 = 0, accel LSBs dropped). Activation sends no status report when the Nunchuk is already attached. Like a real M+, it mirrors the attached Nunchuk's ID bytes 4/0/5 at `0xF6`/`0xF8`/`0xF9` and its plaintext calibration at `0x40`-`0x4F` (Wii Sports Resort Archery checks these and otherwise asks to connect a Nunchuk). User-verified in Dolphin: Mario Kart Wii and WSR Archery. `test/nunchuk_dolphinbar_test.py` (Dolphin closed) exercises all of this through the DolphinBar (passthrough added after Wii Sports Resort did not see the Nunchuk; whole script ALL PASS twice); web-to-HID latency median ~106 ms including the HTTP round trip. DolphinBar quirk: it parses mode-0x3D extension bytes 0/1 as buttons, so stick values containing HOME (byte 1 bit 7) plus a D-pad bit can switch the bar's mode (HOME+UP = mode 3); a real Nunchuk in 0x3D would do the same.

  **Limitation: the fake Nunchuk is confirmed fully working in only one game (Wii Sports Resort Archery) and only in Dolphin (DolphinBar mode 4). Mario Kart Wii responds to the stick and C/Z but Dolphin warns "encryption key unsupported". Not tested on a real Wii/Wii U or in other games.**
- `NEXTMOTE_ENABLE_IMU_MOTION` in `main/wiimote_accel.h` (default 1) can be set to 0 to skip BNO08x startup and the sensor HID stream for isolation testing. Tasks use default FreeRTOS placement; an earlier explicit core split (comm on core 0, sensors/web on core 1) was removed because it made web access drop for several seconds at a time.
- `main/wiimote_identity.c` contains Bluetooth address formatting.
- `main/wiimote_app.c` publishes a primary SDP Device ID record with Nintendo VID/PID `057e:0306`, starts hidden, and opens a 30-second Limited window from web SYNC. `main/wiimote_discovery.c` respects nextMote's IAC capacity and rearms inquiry while alternating codes. The legacy PIN uses the reversed host address without inferring pairing mode from `min_16_digit`. Standard five-cycle GIAC/LIAC inquiry passed 5/5 with CoD `0x002504`; a Wii U/vWii connection, Plus/Minus input, and pointer visibility have since been observed. Full wiimote feature/game parity and reconnect stability remain unverified. Temporary 1+2 guest pairing is unsupported.

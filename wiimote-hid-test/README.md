# ESP32 Bluetooth Classic Wii Remote HID test

This ESP-IDF project configures the original ESP32 as a Bluetooth Classic HID peripheral using Bluedroid. It advertises the `Nintendo RVL-CNT-01` name and handles the legacy PIN flow tested with DolphinBar SYNC. It remains a partial, experimental Wii Remote emulation.

## Verified on ESP32-WROOM + Mayflash DolphinBar (mode 4)

- Legacy PIN pairing/authentication and both HID channels open.
- The bar selects input report `0x30`, sends LED/status requests, and receives responses.
- Wi-Fi HTTP controller page loads; button POSTs update the ESP32's button state.
- Web-generated core-button reports have been observed at the DolphinBar USB slot with the slot watcher.
- A cold connection was observed with serial log `WIIREMOTE_HID_CONNECTED peer=...` and DolphinBar slot status.

The DolphinBar transport and button-report path are verified. The user reports Mario Kart Wii accepts ESPmote A input after a game restart on this Dolphin + DolphinBar mode 4 + ESP32 setup. After the trace-driven build below was flashed, the user also reported being able to enter both Mario Kart Wii and Wii Sports. These are narrow, user-reported results; full gameplay, all controls, other games, real Wii hardware, and Wii U are not certified. Wii Sports Resort is future work and is not implemented or tested; its MotionPlus requirements are outside the current scope.

The serial trace shows Mario Kart Wii reads EEPROM address `0x1770`, length 1. Dolphin's reference behavior is invalid-address error 8; ESPmote returned `00 00 F8 17 70 00 ...` (metadata `0xf8`: size-minus-one 15, error 8). The host then changed report mode and requested status. The trace also shows feature report `0x1A` with ACK requested; firmware now accepts and success-ACKs it. No calibration read at `0x0016` was observed in that trace. Mario Kart Wii does not require a Nunchuk; the earlier Nunchuk inference was incorrect. Flashing resets the HID link; press SYNC if it no longer reconnects.

## Current behavior and missing real-Wiimote features

The Wi-Fi page provides core buttons A, B, 1, 2, D-pad, Plus, Minus and Home, plus A+B and 1+2 combinations. It sends held-button state and clears bits on release. SSID/password are local `sdkconfig` values configured through `idf.py menuconfig`; `sdkconfig` is ignored and must never be committed.

The same page shows a live SEN0158 point plot and raw X/Y values from the native ESP-IDF I2C driver. On `wiimote-ir-pointer`, valid camera point records are passed into HID report `0x33` after the host enables IR. After the mirrored-axis build was flashed, the user confirmed pointer and button operation in Wii Sports and Mario Kart. Pointer movement remains twitchy and has not been smoothed. If the camera probe fails at startup, the endpoint reports `detected:false`; see [IR_CAMERA_STATUS.md](../IR_CAMERA_STATUS.md) for the reset/SYNC diagnostic path. Camera/MPU combination is untested. See also [ASSEMBLY_GUIDE.html](../ASSEMBLY_GUIDE.html); for the guide's 5 V wiring, keep the level shifter in the SDA/SCL path.

On `imu-addon`, the BNO08x uses the ESP-IDF `rinku404/bno085` component on the shared GPIO21/22 I2C bus. It probes `0x4B` and `0x4A`, enables the rotation vector at 100 Hz, and exposes roll/pitch/yaw at `/api/imu`; the web page shows an orientation cube as in the legacy camera app. The current component requires BNO INT on GPIO27 and RST on GPIO26. BNO motion is displayed only; real acceleration is not yet sent in Wii Remote HID reports. Camera-only or IMU-only startup is supported in code; simultaneous sensor hardware remains to be verified.

### IMU-only orientation check

The IMU service shares the GPIO21/22 I2C bus but starts independently of camera detection. For an IMU-only bench test, power off the ESP32 and connect only the GY-BNO08X: VCC/VIN to 3V3, GND to common ground, SDA/SCL to GPIO21/22, RST to GPIO26, and active-low INT to GPIO27. Leave the camera disconnected. Keep PS0/PS1 in I2C mode and AD0 at its default; firmware probes `0x4B` and `0x4A`.

Build and flash this branch, power up, and open the ESPmote page. The camera panel may say “Camera not detected”; the IMU panel should show the detected address and then live roll, pitch, and yaw. `GET /api/imu` returns `detected`, `available`, `address`, `roll`, `pitch`, and `yaw`. Rotate the board by hand to confirm the values change. This tests orientation display only; BNO acceleration is not yet connected to Wii Remote HID reports.

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

The user reports being able to enter Mario Kart Wii and Wii Sports with the prior flashed build. On `wiimote-ir-pointer`, the user confirmed that pointer and buttons work in both Wii Sports and Mario Kart after the mirrored-axis build was flashed. Pointer movement remains twitchy. These are setup-specific results, not complete gameplay or general compatibility validation. The synthetic accelerometer remains stationary. The report watcher decodes buttons from reports `0x30`-`0x33`. Wii Sports Resort remains future work; MotionPlus support is not implemented.

The firmware does **not** yet provide:

- Physical GPIO/button input.
- A real Nunchuk, Classic Controller or other extension; no analog stick is available.
- Live accelerometer/motion data, smoothed pointer behavior across games, speaker output, and rumble. Mirrored-axis pointer and button operation is confirmed in Wii Sports and Mario Kart only.
- Complete EEPROM/calibration and extension-I2C register behavior. The firmware returns a valid synthetic calibration map and emulates only the IR register behavior seen in the trace; it is not a full EEPROM/I2C device model. Memory reads are queued and paced in 10 ms intervals between 16-byte reports.
- Full game-control coverage beyond the user-reported Mario Kart Wii and Wii Sports entry tests, real Wii/Wii U compatibility, persistent reconnects, and soak tests remain unverified.

Some games require extension or motion data and will not work with this implementation. The current FIFA 15 prompt explicitly mentions the Nunchuk stick, so that title is expected to require Nunchuk-compatible extension reports; changing core button masks or sending A alone cannot satisfy that requirement. Even a button-only title may require additional Wiimote initialization/report handling beyond pairing and a button report.

## Build and flash

Requires ESP-IDF 6.0.x. Use an ESP32 target and the serial port for your board:

```bash
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

The project defaults in `sdkconfig.defaults` enable Bluetooth Classic HID-device mode, set the advertised name to `Nintendo RVL-CNT-01`, enable limited discoverability to match the remote's SYNC-mode inquiry behavior, and increase the SDP attribute/padding buffers to publish the descriptor. The limited discoverability option can be changed in `idf.py menuconfig` under **HID Example Configuration**.

### Quick iteration

- Work from `firmware/wiimote-hid-test` in the ESPmote repository, not the separate legacy `espWiimote` Arduino project.
- For normal source edits, use incremental `idf.py build`; reserve `idf.py fullclean` for stale build/configuration problems.
- On the tested ESP32, flash at 115200 baud: `idf.py -p /dev/ttyUSB0 -b 115200 flash`. If esptool reports that the stub failed to start, no image was written; resync with `python -m esptool --chip esp32 -p /dev/ttyUSB0 -b 57600 --no-stub read-mac`, then retry the flash.
- A flash resets the HID connection; press DolphinBar SYNC again. Verify `/api/status` and `/api/camera` at the IP printed by the firmware. The current camera initialization uses the DFRobot default sensitivity profile.

## Pairing and logs

Set the DolphinBar to mode 4 and press SYNC while the ESP32 is powered and advertising. Expected serial evidence includes:

```text
READY identity=Nintendo RVL-CNT-01; awaiting external DolphinBar SYNC
legacy PIN request
legacy PIN reply result=ESP_OK
authentication status=0 peer=HJZ WiiMote Convertor
WIIREMOTE_HID_CONNECTED peer=...
report mode=0x30
LED state=...
```

The `CoD=0x002504` readback confirms the Limited Discoverable service bit is set. A general Bluetooth inquiry may not show this device because it answers the limited inquiry access code, as intended for Wii Remote SYNC behavior.

## Protocol research and acknowledgements

The firmware protocol behavior was implemented in our own code after consulting:

- [WiiBrew: Wiimote](https://wiibrew.org/wiki/Wiimote) for community-documented reports, memory spaces and extension initialization.
- [WiiUse](https://github.com/wiiuse/wiiuse), especially `src/wiiuse.h`, `src/wiiuse_internal.h`, and `src/io.c`, for button masks, report identifiers, memory commands and the common initialization handshake. WiiUse is a GPL-3.0-or-later project; no WiiUse library or source files are linked/copied into this firmware.
- [Dolphin Emulator](https://github.com/dolphin-emu/dolphin), especially `Source/Core/Core/HW/WiimoteCommon/WiimoteConstants.h`, `WiimoteReport.h`, `Source/Core/Core/HW/WiimoteReal/WiimoteReal.cpp`, and `Source/Core/Core/HW/WiimoteEmu/EmuSubroutines.cpp`, for packet structure, error codes and memory read/write behavior. Dolphin is GPL-2.0-or-later; no Dolphin source files are linked/copied into this firmware.

This is protocol-reference attribution, not a claim of code reuse. Any future copying/adaptation of source must be separately reviewed for license obligations and identified in the relevant files.

The IMU branch obtains BNO08x support from `rinku404/bno085` v1.2.0 through ESP-IDF Component Manager. The component and its included CEVA SH2 library are Apache-2.0; the dependency version is pinned in `dependencies.lock`.

## Implementation notes

- `main/wiimote_app.c` contains Classic Bluetooth setup, pairing, GAP/HID event callbacks, and app startup.
- `main/wiimote_state.c` owns synchronized connection, button, LED, IR, and report-mode state.
- `main/wiimote_protocol.c` defines supported report IDs and common protocol checks.
- `main/wiimote_reports.c` encodes and sends input, status, acknowledgement, and memory replies.
- `main/wiimote_commands.c` dispatches host output reports to their capability handlers.
- `main/wiimote_memory.c` emulates EEPROM/I2C reads and writes for initialization.
- `main/wiimote_controls.c` handles web button input and the optional fake-button demo.
- `main/ir_camera_service.c` initializes and polls the SEN0158 camera for the web preview.
- `main/sensor_i2c_bus.c` creates and serializes access to the shared camera/IMU I2C bus.
- `main/imu_service.c` initializes the BNO08x and publishes rotation-vector orientation.
- `main/wiimote_descriptor.c` contains the HID report descriptor.
- `main/wiimote_identity.c` contains Bluetooth address formatting.
- Pairing PIN handling is currently tuned for the tested DolphinBar SYNC flow. Treat 1+2 pairing and console interoperability as unverified.

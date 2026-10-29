# IR camera integration status

Updated 29 October 2026 on `motion-addon`, merged with `main` commit `0811927`.

## Implemented

- The DFRobot SEN0158 camera uses ESP-IDF's native I2C master driver. There is no Arduino `Wire` API or DFRobot library dependency in the firmware.
- Startup probes I2C address `0x58`, uses the DFRobot default initialization profile (`0x06=0x90`, `0x08=0xC0`, `0x1A=0x40`), and reads four raw X/Y points from each 16-byte frame.
- The default ESP32-WROOM bus is GPIO21 SDA / GPIO22 SCL at 100 kHz. Camera polling runs every 50 ms when the Wi-Fi controller is enabled.
- The existing virtual-button page now includes a live point plot and coordinate table. `GET /api/camera` returns `detected`, `available`, and four `{x,y}` values; the page refreshes these values every 120 ms.
- Camera startup is optional. If no device responds at `0x58`, initialization logs a warning, releases the I2C resources, publishes `detected:false`, and continues Bluetooth/Wi-Fi/web startup without creating the camera polling task.
- Read failures are logged and do not stop the Bluetooth HID or web controller.

## Hardware check

On 29 October, the user connected the SEN0158 with VCC at 3.3 V, yellow SDA mapped to GPIO21, and green SCL mapped to GPIO22. The live `/api/camera` response reported `detected:true`, `available:true`, and four non-empty points (sample: P0 `(653,298)`, P1 `(623,302)`, P2 `(644,352)`, P3 `(213,318)`). This verifies camera detection and frame decoding on this board.

An experimental High sensitivity profile was checked in the legacy firmware: remote-control IR produced transient points, while the DolphinBar produced none in a 10-second sample. A subsequent ESP-IDF build with High sensitivity was reported to run Wii Sports with two visible points; `/api/camera` returned `(729,509)` and `(294,488)`. The current modular ESP-IDF firmware has been flashed with the DFRobot default profile, so game and point behavior should be retested after reconnecting.

The user later reported that removing camera power changed the page to “Camera not detected,” and restoring power returned camera detection. A frame showing `1023` for all points means no IR points were found; it does not mean the camera is disconnected.

The copied assembly guide documents the 5 V camera wiring through the bidirectional level shifter. Do not use 5 V camera power with SDA/SCL connected directly to ESP32 GPIO; use the shifter for that configuration.

The point coordinates are passed through as raw values from 0 to 1023. A coordinate of 1023 means that point is empty. `available` means the latest full frame read succeeded; it does not mean that all four points contain a detected source.

The native driver implements the sensor protocol and bit layout documented by DFRobot's [DFRobotIRPosition library](https://github.com/DFRobot/DFRobotIRPosition). Its source header identifies an LGPL license but does not specify a version, and the installed legacy package has no license file. The firmware does not vendor or link that Arduino library; preserve this attribution and verify the upstream terms before redistributing derived work.

## Not implemented or not verified

- Repeated/long-term operation, camera connection when it is absent at ESP32 startup, 5 V plus level-shifter wiring, and combined camera/MPU operation have not been tested. Power cycling the camera after successful initialization was reported to clear and restore detection.
- The user reports that the camera and motion sensor worked separately in the legacy setup, but they were not tested together. The legacy assembly guide identifies the motion board as a GY-BNO08X; no BNO/MPU code is included in this branch.
- This branch displays camera points for diagnostics only. It does not encode the points into Wii Remote IR HID reports or move a game pointer.
- No calibration, filtering, smoothing, point association, or higher-level tracking is implemented.
- The camera module currently owns I2C port 0 on GPIO21/22. If another I2C sensor is added later, it must share this bus instead of trying to initialize a second master on the same pins; combined sensor operation remains untested.
- The merged Wii Remote report path still uses synthetic stationary accelerometer values and empty IR points; camera measurements are not yet wired into those reports.

## Camera-only wiring

Use the ESP32-WROOM-32 DevKit pinout described in [ASSEMBLY_GUIDE.html](./ASSEMBLY_GUIDE.html). This section is the camera subset only; omit all BNO08X wiring.

Power off and unplug the ESP32 before wiring. Use a bidirectional I2C level shifter with a shared ground:

| From | To | Purpose |
|---|---|---|
| ESP32 `VIN/5V` | Shifter `HV` and camera red `VCC` | 5 V camera side |
| ESP32 `3V3` | Shifter `LV` | 3.3 V ESP32 side |
| ESP32 `GND` | Shifter `GND` and camera black `GND` | Common ground |
| ESP32 `GPIO21` | Shifter `LV1` | SDA, low-voltage side |
| ESP32 `GPIO22` | Shifter `LV2` | SCL, low-voltage side |
| Shifter `HV1` | Camera yellow `SDA` | SDA, camera side |
| Shifter `HV2` | Camera green `SCL` | SCL, camera side |

Keep the camera's SDA/SCL on the shifter's HV side; do not connect those wires directly to ESP32 GPIO. The legacy guide describes the camera's operating range as 3.3-5 V but uses 5 V camera-side wiring with level shifting. Follow the module and board markings, and verify that the ESP32 board's `VIN/5V` rail is powered before connecting the camera.

## Build and view

From `firmware/wiimote-hid-test`, build for the original ESP32 target:

```bash
idf.py set-target esp32
idf.py build
```

After a future hardware test, open the ESPmote page at the IP printed by the firmware. The camera plot and point table appear below the virtual buttons. The endpoint can also be inspected directly at `/api/camera`.

The merged firmware has been built and flashed. Camera acquisition was verified at 3.3 V; the guide's 5 V/level-shifter assembly and longer-term camera operation still require validation.

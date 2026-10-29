# IR camera integration status

Updated 29 October 2026 on `wiimote-ir-pointer`, based on local `main` at `10f771e`.

## Implemented

- The DFRobot SEN0158 camera uses ESP-IDF's native I2C master driver. There is no Arduino `Wire` API or DFRobot library dependency in the firmware.
- Startup probes I2C address `0x58`, uses the DFRobot default initialization profile (`0x06=0x90`, `0x08=0xC0`, `0x1A=0x40`), and reads four raw X/Y points from each 16-byte frame.
- The default ESP32-WROOM bus is GPIO21 SDA / GPIO22 SCL at 100 kHz. Camera polling runs every 50 ms when the Wi-Fi controller is enabled.
- The existing virtual-button page now includes a live point plot and coordinate table. `GET /api/camera` returns `detected`, `available`, and four `{x,y}` values; the page refreshes these values every 120 ms.
- On `wiimote-ir-pointer`, the camera's four raw 3-byte point records are copied into the 12 IR bytes of HID report `0x33` when the host has selected that report and enabled both IR stages. Camera changes are sent as they arrive; continuous reporting follows the host's request.
- Camera startup is optional. If no device responds at `0x58`, initialization logs a warning, releases the I2C resources, publishes `detected:false`, and continues Bluetooth/Wi-Fi/web startup without creating the camera polling task.
- Read failures are logged and do not stop the Bluetooth HID or web controller.

## Hardware check

On 29 October, the user connected the SEN0158 with VCC at 3.3 V, yellow SDA mapped to GPIO21, and green SCL mapped to GPIO22. The live `/api/camera` response reported `detected:true`, `available:true`, and four non-empty points (sample: P0 `(653,298)`, P1 `(623,302)`, P2 `(644,352)`, P3 `(213,318)`). This verifies camera detection and frame decoding on this board.

An experimental High sensitivity profile was checked in the legacy firmware: remote-control IR produced transient points, while the DolphinBar produced none in a 10-second sample. A subsequent ESP-IDF build with High sensitivity was reported to run Wii Sports with two visible points; `/api/camera` returned `(729,509)` and `(294,488)`. The current modular ESP-IDF firmware has been flashed with the DFRobot default profile, so game and point behavior should be retested after reconnecting.

On the current modular firmware with the DFRobot default sensitivity profile, the user repeatedly reports that after an ESP32 power cycle all points show `1023` until DolphinBar SYNC is pressed, after which locations register. The camera service initializes during app startup and polls every 50 ms; the HID-open/SYNC path does not start or reinitialize it. A read-only sample after one report showed `connected:true`, `detected:true`, `available:true`, but all points were empty at that instant, so samples need to be captured during the transition. The cause is not established.

Immediately after flashing the pointer branch, `/api/camera` first reported `detected:false` and `available:false`. The user reset the ESP32 and camera detection returned; a later sample reported two points `(416,468)` and `(162,427)` with HID connected. The user then confirmed the Wii Sports pointer appeared and tracked camera movement. This verifies basic pointer operation in that setup. The initial probe failure/reset relationship is still unexplained.

The user later reported that removing camera power changed the page to “Camera not detected,” and restoring power returned camera detection. A frame showing `1023` for all points means no IR points were found; it does not mean the camera is disconnected.

## SYNC-dependent point detection: diagnostic path

Use this sequence before changing sensitivity, wiring, or firmware:

1. After an ESP32 power cycle and before pressing SYNC, check `GET /api/status` and `GET /api/camera`. If `wifi:true` and `detected:true, available:true`, the webserver and I2C frame reads are running. Four `(1023,1023)` values mean no IR blobs were reported in that frame; they do not prove camera failure. If `detected:false`, capture the boot log and resolve the camera probe/initialization error first.
2. Before SYNC, point a TV remote directly at the camera and hold a button while watching `/api/camera`. If coordinates appear, the camera and polling path work before SYNC; investigate whether the DolphinBar's IR source is active/visible before sync, its mode, power, aim, and distance.
3. If the remote also produces no points before SYNC but points appear after SYNC, capture a serial log from boot through the SYNC event. The expected camera initialization log occurs at startup, before HID open. Add temporary, rate-limited logging of raw 16-byte camera frames before and after SYNC to determine whether frame data changes or only point decoding/display changes.
4. Compare the same camera position and IR source before/after SYNC. A phone camera can show that an emitter is on, but does not prove its intensity, wavelength, or what reaches the SEN0158. Do not raise sensitivity or rewire until the before/after frame comparison identifies which side changes.

The copied assembly guide documents the 5 V camera wiring through the bidirectional level shifter. Do not use 5 V camera power with SDA/SCL connected directly to ESP32 GPIO; use the shifter for that configuration.

The point coordinates are passed through as raw values from 0 to 1023. A coordinate of 1023 means that point is empty. `available` means the latest full frame read succeeded; it does not mean that all four points contain a detected source.

The native driver implements the sensor protocol and bit layout documented by DFRobot's [DFRobotIRPosition library](https://github.com/DFRobot/DFRobotIRPosition). Its source header identifies an LGPL license but does not specify a version, and the installed legacy package has no license file. The firmware does not vendor or link that Arduino library; preserve this attribution and verify the upstream terms before redistributing derived work.

## Not implemented or not verified

- Repeated/long-term operation, camera connection when it is absent at ESP32 startup, 5 V plus level-shifter wiring, and combined camera/MPU operation have not been tested. Power cycling the camera after successful initialization was reported to clear and restore detection.
- The user reports that the camera and motion sensor worked separately in the legacy setup, but they were not tested together. The legacy assembly guide identifies the motion board as a GY-BNO08X; no BNO/MPU code is included in this branch.
- Raw camera points are sent in HID report `0x33` on the pointer branch. After the mirrored-axis build was flashed, the user confirmed that both pointer and buttons work in Wii Sports and Mario Kart, with pointer directions corrected. The pointer remains twitchy; smoothing, other games, and long-duration behavior remain unverified. A later API sample showed `available:true` but all points empty, so coordinate reporting may be intermittent.
- No calibration, filtering, smoothing, point association, or higher-level tracking is implemented.
- The camera module currently owns I2C port 0 on GPIO21/22. If another I2C sensor is added later, it must share this bus instead of trying to initialize a second master on the same pins; combined sensor operation remains untested.
- Accelerometer values remain stationary placeholders. No camera coordinate transform, smoothing, calibration, or point association is implemented.

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

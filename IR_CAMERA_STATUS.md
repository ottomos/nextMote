# Camera and IMU integration status

Updated 29 October 2026 on `imu-addon`, based on local `main` at `bcd82a1`.

## Implemented

- The DFRobot SEN0158 camera uses ESP-IDF's native I2C master driver. There is no Arduino `Wire` API or DFRobot library dependency in the firmware.
- Startup probes I2C address `0x58`, uses the DFRobot default initialization profile (`0x06=0x90`, `0x08=0xC0`, `0x1A=0x40`), and reads four raw X/Y points from each 16-byte frame.
- The shared ESP32-WROOM I2C bus uses GPIO21 SDA / GPIO22 SCL at 100 kHz. Camera polling runs every 50 ms when the Wi-Fi controller is enabled; the BNO08x service is scheduled every 10 ms.
- The existing virtual-button page now includes a live point plot and coordinate table. `GET /api/camera` returns `detected`, `available`, and four `{x,y}` values; the page refreshes these values every 120 ms.
- The page also shows BNO08x roll, pitch, and yaw from `GET /api/imu`. The IMU is probed at `0x4B` and `0x4A`, matching the legacy tracker; the service requests the rotation vector at 100 Hz and converts its quaternion to Euler angles.
- Camera points are passed into the IR portion of HID report `0x33` on the pointer branch. Both axes are mirrored in HID only; the camera plot continues to show raw sensor coordinates.
- Camera startup is optional. If no device responds at `0x58`, initialization logs a warning, publishes `detected:false`, and continues. The shared bus remains initialized so an IMU connected without a camera can still be probed.
- Read failures are logged and do not stop the Bluetooth HID or web controller.

## Hardware check

On 29 October, the user connected the SEN0158 with VCC at 3.3 V, yellow SDA mapped to GPIO21, and green SCL mapped to GPIO22. The live `/api/camera` response reported `detected:true`, `available:true`, and four non-empty points (sample: P0 `(653,298)`, P1 `(623,302)`, P2 `(644,352)`, P3 `(213,318)`). This verifies camera detection and frame decoding on this board.

An experimental High sensitivity profile was checked in the legacy firmware: remote-control IR produced transient points, while the DolphinBar produced none in a 10-second sample. A subsequent ESP-IDF build with High sensitivity was reported to run Wii Sports with two visible points; `/api/camera` returned `(729,509)` and `(294,488)`. The current modular ESP-IDF firmware has been flashed with the DFRobot default profile, so game and point behavior should be retested after reconnecting.

On the current modular firmware with the DFRobot default sensitivity profile, the user repeatedly reports that after an ESP32 power cycle all points show `1023` until DolphinBar SYNC is pressed, after which locations register. The camera service initializes during app startup and polls every 50 ms; the HID-open/SYNC path does not start or reinitialize it. A read-only sample after one report showed `connected:true`, `detected:true`, `available:true`, but all points were empty at that instant, so samples need to be captured during the transition. The cause is not established.

Immediately after flashing the pointer branch, `/api/camera` first reported `detected:false` and `available:false`. The user reset the ESP32 and camera detection returned; a later sample reported two points `(416,468)` and `(162,427)` with HID connected. The user then confirmed the Wii Sports pointer appeared and tracked camera movement. This verifies basic pointer operation in that setup. The initial probe failure/reset relationship is still unexplained.

The user later reported that removing camera power changed the page to “Camera not detected,” and restoring power returned camera detection. A frame showing `1023` for all points means no IR points were found; it does not mean the camera is disconnected.

## BNO08x IMU orientation

The legacy `espWiimote` project used the SparkFun BNO08x Arduino library with reset on GPIO26, no INT wire, a 10 ms rotation-vector interval, and address fallback `0x4B`/`0x4A`. The ESP-IDF `imu-addon` branch uses the `rinku404/bno085` ESP-IDF component instead. This component requires the active-low INT/data-ready pin, so the current wiring adds BNO INT → GPIO27 while keeping RST → GPIO26.

The camera and BNO share one ESP-IDF I2C master on GPIO21/22. The bus lock keeps the camera's register-select/frame read together and serializes that transfer with BNO SH-2 service transactions. Camera initialization failure does not tear down the shared bus.

`GET /api/imu` returns `detected`, `available`, `address`, `roll`, `pitch`, and `yaw`. The web page displays the three angles and a rotating orientation cube. With the camera disconnected, the camera panel may report not detected while the IMU panel can still report readings. This branch is for orientation display/testing: BNO acceleration is not yet incorporated into Wii Remote HID accelerometer bytes.

### IMU-only wiring for the test

Power off and disconnect USB before wiring. Connect BNO VCC/VIN to ESP32 3V3, GND to common GND, SDA to GPIO21, SCL to GPIO22, RST to GPIO26, and active-low INT to GPIO27. Leave PS0/PS1 in I2C mode and leave AD0 at its default; firmware probes both `0x4A` and `0x4B`. Leave the camera disconnected for this test if desired. The firmware remains Wi-Fi enabled and should serve the orientation panel even when the camera is absent.

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
- The user previously reported that the camera and motion sensor each worked separately in the legacy setup, but they were not tested together. The `imu-addon` branch now includes BNO08x orientation acquisition and web display; current-hardware testing is still pending.
- Raw camera points are sent in HID report `0x33` on the pointer branch. After the mirrored-axis build was flashed, the user confirmed that both pointer and buttons work in Wii Sports and Mario Kart, with pointer directions corrected. The pointer remains twitchy; smoothing, other games, and long-duration behavior remain unverified. A later API sample showed `available:true` but all points empty, so coordinate reporting may be intermittent.
- No calibration, filtering, smoothing, point association, or higher-level tracking is implemented.
- The shared sensor I2C bus owns port 0 on GPIO21/22. Additional sensors must use this bus rather than initialize a second master on those pins; simultaneous camera/BNO hardware operation remains unverified.
- Accelerometer values remain stationary placeholders. No camera coordinate transform, smoothing, calibration, or point association is implemented.

## Camera-only wiring

Use the ESP32-WROOM-32 DevKit pinout described in [ASSEMBLY_GUIDE.html](./ASSEMBLY_GUIDE.html). This section is the camera subset only; omit all BNO08X wiring.

Power off and unplug the ESP32 before wiring. The camera was tested powered at 3.3 V with direct I2C, so a level shifter is not required for that setup:

| From | To | Purpose |
|---|---|---|
| ESP32 `3V3` | Camera red `VCC` | Camera power (within SEN0158 3.3–5 V range) |
| ESP32 `GND` | Camera black `GND` | Common ground |
| ESP32 `GPIO21` | Camera yellow `SDA` | Direct 3.3 V I2C data |
| ESP32 `GPIO22` | Camera green `SCL` | Direct 3.3 V I2C clock |

If powering the camera from 5 V instead, use the level shifter: ESP32 3V3 to LV, ESP32 5V to HV, GPIO21/22 to LV1/LV2, and camera SDA/SCL to HV1/HV2. Do not connect 5 V camera data lines directly to ESP32 GPIO.

## Build and view

From `firmware/wiimote-hid-test`, build for the original ESP32 target:

```bash
idf.py set-target esp32
idf.py build
```

Open the ESPmote page at the IP printed by the firmware. The camera plot and point table appear below the virtual buttons; the BNO08x orientation panel shows roll/pitch/yaw and a rotating cube. Inspect `/api/camera` for IR points and `/api/imu` for IMU state and Euler angles. For an IMU-only check, disconnect the camera and follow the BNO08x wiring in [ASSEMBLY_GUIDE.html](./ASSEMBLY_GUIDE.html).

The firmware build and web support are implemented. Camera acquisition at 3.3 V is verified. BNO08x hardware on the current ESP32 wiring, the new INT=GPIO27 connection, and combined camera/IMU operation still require testing.

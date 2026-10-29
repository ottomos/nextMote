# ESPmote

An ESP32 Bluetooth Classic HID-device prototype that presents a Wii Remote-like HID identity to a Mayflash DolphinBar. This is experimental firmware, **not a complete Wii Remote replacement**. Bluetooth pairing, HID channels, status/LED exchange, a Wi-Fi web controller and button reports have been exercised through the DolphinBar. The user reports that Mario Kart Wii accepts web-generated A input after a game restart, and that the current trace-driven build allows entry into both Mario Kart Wii and Wii Sports in the tested Dolphin + DolphinBar mode 4 setup. These are narrow, user-reported results, not general compatibility certification or proof of full gameplay support.

## Hardware and software

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

The default configuration advertises `Nintendo RVL-CNT-01` and enables limited-discoverable mode. It also enlarges the SDP buffers so the HID report descriptor is published. If DolphinBar behavior differs, `EXAMPLE_LIMITED_DISCOVERABLE` can be toggled through `idf.py menuconfig` under **HID Example Configuration**.

## Cold-boot connection test

1. Close Dolphin or other software that may claim the DolphinBar's hidraw devices.
2. Power down the ESP32 and unplug the bar.
3. Start the live test before plugging either device in:

   ```bash
   python3 -m pip install pyserial
   python3 tools/espmote_live_test.py
   ```

4. Plug in the DolphinBar, set it to mode 4, and power the ESP32.
5. Press the DolphinBar's SYNC button when the terminal is ready.
6. In a second terminal, monitor the ESP32's occupied slot (replace `N` with the reported slot):

   ```bash
   python3 tools/dolphinbar_button_watch.py --slot N --seconds 60
   ```

   The fake-button branch sends `FAKE_BUTTON press NAME` / release logs on ESP32 serial. The Wi-Fi web-control branch sends live press/release states from the page instead of cycling.

The monitor shows ESP32 serial events and bar USB slot occupancy together. A successful test should show a legacy PIN request, authentication success, `WIIREMOTE_HID_CONNECTED`, a configured report mode, and an occupied DolphinBar slot. Use `Ctrl-C` to stop. Set `--port /dev/ttyUSB0` if automatic serial-port detection chooses the wrong device; use `--seconds 600` for a ten-minute observation window.

Useful additional probes:

```bash
python3 tools/dolphinbar_probe.py
python3 tools/dolphinbar_button_watch.py --slot 1 --seconds 60
python3 tools/dolphinbar_sync_test.py --no-inquiry
```

The slot probe and watcher are read-only with respect to pairing. Do not run them at the same time as Dolphin: both need access to the same HID endpoints. In the fake-button demo, choose the ESP32's slot with `--slot N`; run the watcher to see each press/release label.

## Current scope and limitations

- **Verified through the DolphinBar:** Classic Bluetooth pairing/authentication, HID open, host report-mode and LED commands, status request/reply, and web-generated core-button reports at the bar's USB HID slot. The captured Mario Kart Wii startup trace produced the expected error-8 reply for EEPROM probe `0x1770`, then handled report `0x1A`.
- **Game results reported by the user:** Mario Kart Wii accepted web-generated A input and continued working after a game restart. After the trace-driven compatibility build was flashed, the user also reported being able to enter both Mario Kart Wii and Wii Sports. These results apply to the tested Dolphin + DolphinBar mode 4 + ESP32 setup only. Wii Sports Resort is future work and is not supported or tested; its MotionPlus requirements are not implemented. Full gameplay, all controls, other games, real Wii hardware, and Wii U compatibility remain unverified. Mario Kart Wii does not require a Nunchuk; the earlier Nunchuk inference was incorrect.
- The captured startup trace reads EEPROM `0x1770`; Dolphin's reference implementation rejects it with invalid-address error 8. ESPmote replied `00 00 F8 17 70 00 ...`; the same trace showed output feature report `0x1A`, which ESPmote now accepts and success-ACKs. No calibration read at `0x0016` appeared in that capture.
- The Wi-Fi HTTP page offers A/B/1/2, D-pad, Plus/Minus/Home and A+B/1+2. Credentials must be entered locally via `idf.py menuconfig` under **HID Example Configuration** and stay in ignored `sdkconfig`; never put them in `sdkconfig.defaults`, source, or commits.
- **Camera preview:** this `motion-addon` branch includes native ESP-IDF SEN0158 acquisition and a live plot/table below the virtual buttons. The camera was detected and returned four points with VCC at 3.3 V and SDA/SCL mapped to GPIO21/22. Startup is optional: without a camera at `0x58`, firmware logs the failure and continues Wi-Fi/Bluetooth startup. The preview is diagnostic only; points are not sent in HID reports. See [IR_CAMERA_STATUS.md](./IR_CAMERA_STATUS.md) and the [assembly guide](./ASSEMBLY_GUIDE.html).
- **Wii Sports trace fixes from `main`:** mode `0x33` uses a 17-byte buttons/accelerometer/IR payload; EEPROM `0x0000`-`0x0029` returns duplicated checksum-valid synthetic calibration; I2C writes to IR slave `0x58` are accepted and reads at `0x36` return an empty-point frame; status reflects both IR-enable stages. Mode-`0x30` button updates remain event-driven. See [the trace table](./wiimote-hid-test/README.md#wii-sports-trace-requests-prior-replies-and-corrections).
- **Motion/IR limits:** the camera preview reads real points but is diagnostic only and is not connected to the HID IR fields. Current accelerometer values remain stationary placeholders. No real MPU/BNO input, sensor fusion, IR pointing, or MotionPlus emulation is implemented. Camera and MPU have not been tested together.
- **Not implemented:** physical controls, a real Nunchuk or other extension, real accelerometer/gyro data, real IR HID points, speaker audio, or rumble. EEPROM and I2C behavior remain partial despite the tested-range calibration and IR-register shims; they are not full device emulators. Memory reads are paced in 10 ms intervals between 16-byte reports.
- **Current game scope:** the user reports entering Mario Kart Wii and Wii Sports in the tested Dolphin + DolphinBar mode 4 + ESP32 setup, with previous A-button success in Mario Kart Wii. Wii Sports Resort is future work and is not supported or tested; its MotionPlus requirements are outside the current scope.
- **Still unverified:** full game-control coverage, games beyond the user-reported tests, real Wii, Wii U native/vWii, persistent reconnects, and long-duration soak behavior. HID connection and a button report at the bar do not by themselves establish full compatibility.
- **Source/library provenance:** The firmware does not link or vendor WiiUse or Dolphin code. Protocol details were consulted from WiiBrew's Wiimote protocol documentation, WiiUse's `wiiuse.h`/`io.c`/`wiiuse_internal.h` protocol constants and reference behavior, and Dolphin's `WiimoteConstants.h`, `WiimoteReport.h`, `WiimoteReal.cpp`, and `WiimoteEmu/EmuSubroutines.cpp`. Those repositories are protocol references only; their source files were not copied into this project. See the attribution section in [`wiimote-hid-test/README.md`](wiimote-hid-test/README.md).

See [`wiimote-hid-test/README.md`](wiimote-hid-test/README.md) for firmware details and debugging.

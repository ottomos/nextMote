# ESPmote

An ESP32 Bluetooth Classic HID-device prototype that presents a Wii Remote-like HID identity to a Mayflash DolphinBar. This is experimental firmware, **not a complete Wii Remote replacement**. Bluetooth pairing, HID channels, status/LED exchange, a Wi-Fi web controller and button reports have been exercised through the DolphinBar. Mario Kart Wii now works with web-generated A input in the user's Dolphin setup, including after a game restart; this is a limited setup-specific compatibility result, not a general certification. Memory/init handlers remain partial and calibration data is synthetic.

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

- **Verified through the DolphinBar:** Classic Bluetooth pairing/authentication, HID open, host report-mode and LED commands, status request/reply, and web-generated core-button reports at the bar's USB HID slot. Latest memory/init firmware is built and flashed; its Mario Kart Wii startup trace produced the expected error-8 reply for EEPROM probe `0x1770`, then handled report `0x1A`. User subsequently confirms Mario Kart Wii works with A and still works after restarting the game. Flashing resets the bar link, so press SYNC if the HID connection is down.
- **Game compatibility:** user reports Mario Kart Wii accepts web-generated A input and remains working after the game is restarted. This is a pass for this Dolphin + DolphinBar mode 4 + ESP32 setup, including after closing/restarting the game, not a broad compatibility claim. The exact fix responsible has not been isolated. Mario Kart Wii does not require a Nunchuk; the earlier Nunchuk inference was incorrect.
- The captured startup trace reads EEPROM `0x1770`; Dolphin's reference implementation rejects it with invalid-address error 8. ESPmote replied `00 00 F8 17 70 00 ...`; the same trace showed output feature report `0x1A`, which ESPmote now accepts and success-ACKs. No calibration read at `0x0016` appeared in that capture.
- The Wi-Fi HTTP page offers A/B/1/2, D-pad, Plus/Minus/Home and A+B/1+2. Credentials must be entered locally via `idf.py menuconfig` under **HID Example Configuration** and stay in ignored `sdkconfig`; never put them in `sdkconfig.defaults`, source, or commits.
- **Not implemented:** physical controls; a real Nunchuk or other expansion; valid live accelerometer/motion data; IR camera/pointing; speaker audio; rumble motor output. Memory/init handling is partial: it supplies synthetic calibration bytes, accepts two extension-disable writes, and returns no-extension/NACK for unsupported I2C devices. It is not a complete EEPROM, calibration or I2C/register emulator. This newest memory/init code has built/flashed but is not yet verified against the Mario Kart Wii/Dolphin startup trace. Multi-chunk reads are sent by a dedicated task with 10 ms pacing between 16-byte replies.
- **Not verified:** Dolphin/game compatibility beyond the DolphinBar transport, real Wii, Wii U native/vWii, persistent reconnects and long-duration soak behavior. HID connection and a button report at the bar do not by themselves establish application compatibility.
- **Source/library provenance:** The firmware does not link or vendor WiiUse or Dolphin code. Protocol details were consulted from WiiBrew's Wiimote protocol documentation, WiiUse's `wiiuse.h`/`io.c`/`wiiuse_internal.h` protocol constants and reference behavior, and Dolphin's `WiimoteConstants.h`, `WiimoteReport.h`, `WiimoteReal.cpp`, and `WiimoteEmu/EmuSubroutines.cpp`. Those repositories are protocol references only; their source files were not copied into this project. See the attribution section in [`wiimote-hid-test/README.md`](wiimote-hid-test/README.md).

See [`wiimote-hid-test/README.md`](wiimote-hid-test/README.md) for firmware details and debugging.

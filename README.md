# ESPmote

An ESP32 Bluetooth Classic HID-device prototype that impersonates a Wii Remote and pairs with a Mayflash DolphinBar. This is experimental firmware, **not a complete Wii Remote replacement**: the current milestone verifies pairing, HID setup and status/LED report exchange. Button simulation, motion/IR, and Nintendo console compatibility are not implemented or verified.

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

   The demo sends `FAKE_BUTTON press NAME` / release logs on ESP32 serial and the watcher should report the matching press followed by `[none]`.

The monitor shows ESP32 serial events and bar USB slot occupancy together. A successful test should show a legacy PIN request, authentication success, `WIIREMOTE_HID_CONNECTED`, a configured report mode, and an occupied DolphinBar slot. Use `Ctrl-C` to stop. Set `--port /dev/ttyUSB0` if automatic serial-port detection chooses the wrong device; use `--seconds 600` for a ten-minute observation window.

Useful additional probes:

```bash
python3 tools/dolphinbar_probe.py
python3 tools/dolphinbar_button_watch.py --slot 1 --seconds 60
python3 tools/dolphinbar_sync_test.py --no-inquiry
```

The slot probe and watcher are read-only with respect to pairing. Do not run them at the same time as Dolphin: both need access to the same HID endpoints. In the fake-button demo, choose the ESP32's slot with `--slot N`; run the watcher to see each press/release label.

## Current scope and limitations

- **Verified:** Bluetooth Classic pairing with the DolphinBar, HID channels open, host report-mode/LED setup, and a status request/reply through a DolphinBar slot.
- **Available in the `fake-buttons` branch:** a demo cycles through all 11 core buttons with a 500 ms press / 250 ms neutral release. This is synthetic test input, not physical-button support.
- **Not yet implemented:** accelerometer/gyro, IR pointer, extensions, speaker, rumble, persistent reconnect handling.
- **Not yet verified:** Dolphin's controller UI, Wii, Wii U native/vWii, and long-duration/restart reconnect tests.
- Some host output reports are currently unsupported. The serial log identifies them. HID transport success alone does not mean every application accepts the device.

See [`wiimote-hid-test/README.md`](wiimote-hid-test/README.md) for firmware details and debugging.

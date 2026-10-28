# ESP32 Bluetooth Classic Wii Remote HID test

This ESP-IDF project configures the original ESP32 as a sensor-free Bluetooth Classic HID device using Bluedroid. It advertises as `Nintendo RVL-CNT-01`, handles the legacy PIN pairing used by DolphinBar SYNC, and implements a small subset of the Wii Remote report protocol.

## Current test scope

Verified on ESP32-WROOM + Mayflash DolphinBar (mode 4):

- Legacy PIN pairing and Bluetooth authentication with the bar
- HID channels opened
- Host report-mode and LED configuration received
- Status request received from the bar and status report returned through USB

Not implemented or verified: fake button input, motion/IR, extensions, rumble, speaker, Dolphin UI recognition, Wii/Wii U compatibility, or persistent reconnect behavior. Unsupported host output reports are logged; HID transport success alone does not imply application compatibility.

## Build and flash

Requires ESP-IDF 6.0.x. Use an ESP32 target and the serial port for your board:

```bash
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

The project defaults in `sdkconfig.defaults` enable Bluetooth Classic HID-device mode, set the advertised name to `Nintendo RVL-CNT-01`, enable limited discoverability to match the remote's SYNC-mode inquiry behavior, and increase the SDP attribute/padding buffers to publish the descriptor. The limited discoverability option can be changed in `idf.py menuconfig` under **HID Example Configuration**.

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

## Implementation notes

- `main/wiimote_test.c` contains Classic Bluetooth setup, pairing, HID callbacks, and the minimal report handlers.
- `main/wiimote_descriptor.c` contains the HID report descriptor.
- `main/wiimote_identity.c` contains Bluetooth address formatting.
- Pairing PIN handling is currently tuned for the tested DolphinBar SYNC flow. Treat 1+2 pairing and console interoperability as unverified.

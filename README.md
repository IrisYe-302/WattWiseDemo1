# Wattwise Arduino IDE firmware

This is a standard Arduino IDE sketch for two ESP32-S3 boards. PlatformIO is
not required.

## Open the sketch

Open:

```text
WattwiseFirmware/WattwiseFirmware.ino
```

The `.ino` tab is intentionally small. The firmware implementation is in the
adjacent `Firmware.cpp` tab so Arduino's sketch preprocessor does not rewrite
JavaScript embedded in the dashboard.

Install Espressif's **esp32** board package in Arduino IDE and select the board
definition matching your ESP32-S3. For a generic development board, use
**ESP32S3 Dev Module**. Set the serial monitor to 115200 baud.

The sketch uses only libraries included with the ESP32 Arduino core:

- WiFi
- WebServer
- ESP-NOW

## Upload node 0: smart master

In the `Config.h` tab, use:

```cpp
#define WATTWISE_HOME_ID 0
#define WATTWISE_IS_MASTER 1
```

Upload to the smart/master board. It creates the `Wattwise` Wi-Fi network and
hosts the API at `http://192.168.4.1`.

## Upload node 1: conventional slave

Change the same two lines to:

```cpp
#define WATTWISE_HOME_ID 1
#define WATTWISE_IS_MASTER 0
```

Upload to the conventional/slave board.

## Wiring

Every LED needs its own current-limiting resistor, typically 220-470 ohms.

| Function | GPIO | Boards |
| --- | ---: | --- |
| Grid online LED | 4 | node 0 only |
| Grid outage LED | 5 | node 0 only |
| Appliance 1 - refrigerator (solid) | 6 | both |
| Appliance 2 - protected emergency charger (flashing) | 3 | both |
| Appliance 3 - shed-eligible standby load (flashing) | 14 | both |

GPIO 3 is a strapping pin on some ESP32-S3 boards. If the attached LED circuit
prevents booting, disconnect it during reset or move that appliance output to a
safe GPIO in `Config.h`.

## Demo behavior

1. **Baseline:** GPIO 6 is solid. GPIO 3 flashes as a low-draw but protected
   emergency-device charger. GPIO 14 flashes as ordinary standby consumption.
2. **Grid optimization:** smart node 0 keeps GPIO 6 solid and GPIO 3 flashing,
   but switches GPIO 14 off. Conventional node 1 keeps all three loads.
3. **Grid outage:** node 1 fades its three loads to zero over 20 seconds. Node 0
   immediately sheds GPIO 14 while its solid critical load and protected
   flashing charger fade over 45 seconds.

The accelerated outage durations are configured in `Config.h`.

## Wi-Fi and MAC addresses

- SSID: `Wattwise`
- Password: `wattwise-demo`
- Master API: `http://192.168.4.1`

Node 0 prints its SoftAP MAC at startup. Node 1 prints its station MAC. Once
both nodes are running, the laptop client's `status` command displays both.

If Arduino IDE shows only the ROM boot text, enable the board's **USB CDC On
Boot** option or select the newly enumerated USB serial port after reset.

## Laptop client

Connect the laptop to `Wattwise`, then run:

```sh
python3 tools/wireless_client.py
```

Enter `1`, `2`, or `3` to select a stage. Press Enter to advance. One-shot
commands also work:

```sh
python3 tools/wireless_client.py stage 1
python3 tools/wireless_client.py stage 2
python3 tools/wireless_client.py stage 3
python3 tools/wireless_client.py status
```

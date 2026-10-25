# mqtt2zigbee

[![Build](https://github.com/VadimSurpin/mqtt2zigbee/actions/workflows/build.yml/badge.svg)](https://github.com/VadimSurpin/mqtt2zigbee/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

ESP32-C6 firmware that exposes MQTT controls as **virtual Zigbee endpoints**, so a
Zigbee hub (Yandex Alice, Home Assistant/ZHA, zigbee2mqtt, ...) can see and control
devices that only speak MQTT — for example [Wiren Board](https://wirenboard.com)
relays and sensors — without any extra gateway software.

```
 MQTT broker  <──Wi-Fi──>  ESP32-C6  <──802.15.4──>  Zigbee hub / Alice
 (WB, z2m, ...)          (one Zigbee router
                          with N endpoints)
```

## Features

- One Zigbee **router** node with many endpoints: switches, lights (on/off + level),
  temperature, humidity, pressure, contact and analog sensors.
- **MQTT discovery** page in the web UI with three sources:
  - *Wirenboard* — WB devices without the built-in ones (`system`, `network`, `wb-gpio`, ...);
  - *Wirenboard with built-ins*;
  - *Zigbee2MQTT* — devices published by an existing zigbee2mqtt instance
    (streamed parser, safe for large `bridge/devices` payloads).
  Sources that are not available on the broker are disabled automatically.
- Web UI for Wi-Fi provisioning, MQTT settings, device selection, Zigbee pairing
  (English and Russian).
- Wi-Fi/Zigbee coexistence, OTA-ready partition table with rollback, status LED,
  long-press factory reset.

## Hardware

- ESP32-C6 board with ≥ 8 MB flash (`partitions.csv` uses two 3 MB OTA slots).
- Status LED on GPIO 8 (change `STATUS_LED_GPIO` in `main/main.c`).
- BOOT button on GPIO 9: short press toggles the access point, hold 3 s for factory reset.

## Build

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/v5.4.1/esp32c6/get-started/) **v5.4.1**.

```bash
. $IDF_PATH/export.sh
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

CI builds every push and pull request; tagged releases (`v*`) attach ready-made
firmware binaries.

### Flashing a release

```bash
pip install esptool
esptool.py --chip esp32c6 -p /dev/ttyACM0 write_flash 0x0 mqtt2zigbee-<version>-merged.bin
```

## First run

1. Connect to the Wi-Fi access point **ZigbeeGW** and open <http://192.168.4.1>.
2. Log in with the default credentials `admin` / `gateway` — you are asked to change them.
3. Enter your Wi-Fi and MQTT broker settings.
4. Open **Devices**, pick the source and the controls to expose.
5. Open **Pair** and add the node on your Zigbee hub.

> The hub caches endpoints. After adding or removing exposed controls, restart the
> gateway and re-pair the node.

## Project layout

| Path | Purpose |
|------|---------|
| `main/` | app entry, config storage, device manager |
| `components/zigbee_core` | Zigbee stack glue, endpoint creation |
| `components/device_drivers` | per-type drivers (WB switch/range/sensor, z2m, ...) |
| `components/wb_driver` | Wiren Board topic discovery |
| `components/z2m_driver` | zigbee2mqtt device-list parser |
| `components/web_server`, `web_ui` | HTTP server and frontend |
| `components/esp-zigbee-lib` | vendored Espressif esp-zigbee-lib (Apache-2.0) |
| `tools/mock_server.py` | Flask mock of the web UI for frontend development |

More detail in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Security issues: [SECURITY.md](SECURITY.md).

## License

MIT — see [LICENSE](LICENSE). `components/esp-zigbee-lib` is © Espressif Systems under
Apache-2.0 (see its own `LICENSE`).

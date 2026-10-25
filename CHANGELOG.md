# Changelog

## [1.0.0] - 2026-10-25

### Added
- One Zigbee router node exposing MQTT controls as endpoints (switch, light, temperature,
  humidity, pressure, contact, analog).
- Web UI: Wi-Fi provisioning, MQTT settings, device selection, Zigbee pairing (EN/RU).
- Discovery source selector: Wirenboard / Wirenboard with built-ins / Zigbee2MQTT.
- Zigbee2MQTT device discovery and driver (streamed parser for large device lists).
- Wiren Board built-in device filter.
- OTA-ready partition table with rollback, status LED, factory reset button.
- CI build and tagged-release workflow.

Tested on ESP32-C6 with a Wiren Board controller and WB-MRC relay modules.

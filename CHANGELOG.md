# Changelog

## [1.0.1] - 2026-10-26

### Changed
- Status LED is now off when fully operational (MQTT connected); green was too bright.

### Added
- Wi-Fi page shows the router SSID/IP the gateway is connected to (STA).
- Wi-Fi page control to disable/enable the setup access point (disabling is only
  possible while connected to a router). The setting is persisted, exported as
  `wifi.ap_disabled` in the YAML backup, and kept in sync with the BOOT-button AP toggle.
  If the router is unreachable at boot the AP stays up, so you are never locked out.

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

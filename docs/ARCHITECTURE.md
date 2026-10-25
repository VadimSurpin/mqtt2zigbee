# Architecture

- The ESP32-C6 runs a single Zigbee router. All exposed controls are endpoints of
  that node, registered once at boot (`device_manager_build_zigbee_endpoints`).
  Changing the exposed list therefore requires a restart and re-pairing.
- Configured devices are stored in NVS (`devices_json`) as lines
  `type|topic|manufacturer|model[|subtype]`.
- `device_manager` routes MQTT messages to drivers by topic and Zigbee attribute
  writes back to MQTT.
- Drivers live in `components/device_drivers` and register themselves in
  `device_driver.c`.

## Sources

**Wiren Board** — `wb_driver` listens to `/devices/<dev>/controls/<ctrl>` and its
`/meta/*` topics for a 60 s discovery window. Commands go to `.../on`.

**Zigbee2MQTT** — `z2m_driver` streams the retained `zigbee2mqtt/bridge/devices`
message through a small JSON scanner (the payload can be hundreds of KB) and keeps
only a compact table of exposable properties. State is read from
`zigbee2mqtt/<name>`, commands are published to `zigbee2mqtt/<name>/set`.

## Memory notes

Discovery tables are heap-allocated only while the discovery page is in use.

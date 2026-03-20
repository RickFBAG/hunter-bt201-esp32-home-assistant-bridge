# Home Assistant UX Layer

This folder contains the Home Assistant package and dashboard files that turn the raw MQTT bridge entities into a much simpler daily control UX.

## Included Files

- `packages/hunter_btt201_ui.yaml`
  - helpers
  - program scripts
  - weekly schedule automation
  - summary/status template sensors
- `dashboards/hunter_btt201_dashboard.yaml`
  - main mobile-first control view
  - advanced native Hunter schedules view

## Assumptions

These files assume the bridge uses the default device id:

```c
#define BRIDGE_DEVICE_ID "hunter_bridge"
```

If you changed `BRIDGE_DEVICE_ID`, replace every `hunter_bridge_` entity id in these Home Assistant YAML files.

## HAOS Installation

1. Copy `home_assistant/packages/hunter_btt201_ui.yaml` to `/config/packages/`.
2. Copy `home_assistant/dashboards/hunter_btt201_dashboard.yaml` to `/config/dashboards/`.
3. Ensure `configuration.yaml` includes packages:

```yaml
homeassistant:
  packages: !include_dir_named packages
```

4. Add the YAML dashboard:

```yaml
lovelace:
  dashboards:
    hunter-btt201:
      mode: yaml
      title: Hunter BTT201
      icon: mdi:sprinkler-variant
      show_in_sidebar: true
      filename: dashboards/hunter_btt201_dashboard.yaml
```

5. Restart Home Assistant, or reload YAML-backed helpers/integrations where possible.
6. Open the new `Hunter BTT201` dashboard from the sidebar.

## If The Advanced View Shows "Entity not found"

The bridge now publishes explicit `default_entity_id` values for MQTT discovery so Home Assistant can create predictable raw entity IDs such as:

- `button.hunter_bridge_zone1_start`
- `number.hunter_bridge_zone1_manual_duration`
- `sensor.hunter_bridge_zone1_state`

If you discovered the bridge before this change, Home Assistant may still keep the older auto-generated entity IDs in its entity registry.

In that case:

1. Go to `Settings -> Devices & services -> MQTT`
2. Open the Hunter bridge device
3. Remove the old MQTT-discovered bridge device/entities
4. Reboot the ESP32 bridge or restart MQTT in Home Assistant so discovery is published again

After rediscovery, the `Advanced` dashboard view should line up with the expected raw entity IDs.

## How To Use It

### Everyday use

- Change the minute values in `Default Program`
- There is no separate save button; Home Assistant stores those helper values immediately
- `Run Program` always uses the current values
- Use `Stop All` when you want to interrupt watering immediately
- Set `Zone 1 Minutes`
- Set `Zone 2 Minutes`
- Set `Repeat Count`
- Press `Run Program`

This runs:

- Zone 1
- then Zone 2
- then repeats the full sequence if configured
- Quick presets are available for `5 min`, `15 min`, and `30 min`

### Scheduling

- Turn `Schedule Enabled` on
- Pick one start time
- Toggle the weekdays you want enabled
- Leave disabled the days you do not want the program to run on

The schedule triggers the HA program script. It does not write the native Hunter timer or cycling blocks.

### Advanced use

If you still want the raw bridge controls:

- open the `Advanced` dashboard view
- use the native timer/cycling entities there

## Notes

- The HA program is the recommended primary UX
- The bridge still enforces the 1-hour runtime cap
- The bridge still handles BLE confirmation/fail-safe logic
- The HA schedule does not backfill missed runs after a restart
- Every Hunter BLE session already refreshes the Hunter battery percentage automatically

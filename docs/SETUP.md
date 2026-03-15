# Setup

## 1. Install Prerequisites

- Install VS Code plus the PlatformIO extension, or install PlatformIO Core separately.
- Ensure you have an MQTT broker reachable from both the ESP32 and Home Assistant.
- Ensure the Hunter clock is already correct in the phone app before using schedules.

## 2. Configure Secrets

1. Copy [bridge_secrets.example.h](/c:/Users/rick_/Desktop/Git/ESP32HunterBTT201Bridge/include/bridge_secrets.example.h) to `include/bridge_secrets.h`.
2. Fill in:
   - `BRIDGE_WIFI_SSID`
   - `BRIDGE_WIFI_PASSWORD`
   - `BRIDGE_MQTT_URI`
   - `BRIDGE_MQTT_USERNAME`
   - `BRIDGE_MQTT_PASSWORD`
   - `BRIDGE_DISCOVERY_PREFIX`
   - `BRIDGE_BASE_TOPIC`
   - `BRIDGE_DEVICE_ID`
   - `BRIDGE_DEVICE_NAME`
   - `BRIDGE_HUNTER_MAC`

Leave `BRIDGE_HUNTER_PASSCODE` empty in v1 unless you are deliberately experimenting with `FF81`. The current firmware does not use `FF81` as an unlock flow.

## 3. Build And Flash

### VS Code PlatformIO

1. Open the repo in VS Code.
2. Select the `waveshare_esp32_c6_touch` environment.
3. Build.
4. Flash.
5. Open the serial monitor at `115200`.

### PlatformIO Core

```powershell
pio run -e waveshare_esp32_c6_touch
pio run -e waveshare_esp32_c6_touch -t upload
pio device monitor -b 115200
```

## 4. Home Assistant

1. Add the MQTT integration if it is not already configured.
2. Point it at the same broker configured in `bridge_secrets.h`.
3. Power the ESP32 and wait for MQTT discovery.
4. Home Assistant should create a single discovered device with:
   - per-zone start/stop buttons
   - manual duration numbers
   - timer controls
   - cycling controls
   - battery sensor
   - bridge health sensor

## 5. First Validation

1. Confirm the bridge appears in Home Assistant.
2. Confirm battery percentage is visible.
3. Set Zone 1 manual duration to a small value like `30`.
4. Press Zone 1 start.
5. Confirm state transitions to `starting` then `running`.
6. Press stop and confirm it returns to `idle`.
7. Repeat for Zone 2.

## 6. Schedule Notes

- Schedules are staged locally in the bridge draft state.
- A schedule is only pushed to the Hunter device when you press the matching `Apply` button.
- `FF84` time sync is intentionally not written in v1.
- If a schedule write or read-back does not match, the apply status sensor reports failure and the firmware does not pretend success.


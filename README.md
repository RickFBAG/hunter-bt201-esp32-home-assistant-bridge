# Hunter BTT201 ESP32 Home Assistant Bridge

Local ESP32 bridge for the Hunter BTT201 / Hunter BTT irrigation controller.

This project lets Home Assistant control a Hunter BTT201 over your local network by using an ESP32 as the only BLE client. The Hunter stays a BLE-only device, but Home Assistant gets clean local control through MQTT discovery.

## What This Project Does

- Starts and stops Zone 1 and Zone 2 from Home Assistant
- Enforces a hard 1 hour maximum watering duration
- Exposes Hunter battery percentage
- Supports timer and cycling schedules where protocol evidence is strong enough
- Uses confirmation and bounded retries instead of blind BLE writes
- Fails safe on reboot, disconnect, timeout, and unexpected errors
- Shows passive local status on the Waveshare ESP32-C6 Touch AMOLED 1.8 screen

## Hardware Target

- Hunter BTT201 / Hunter BTT
- Waveshare ESP32-C6 Touch AMOLED 1.8
- Home Assistant OS or any Home Assistant instance with MQTT enabled
- Local MQTT broker, typically Mosquitto

## Why This Architecture

The bridge uses the simplest reliable fully local design:

- ESP32 talks BLE to the Hunter
- Home Assistant talks MQTT to the ESP32
- BLE stays disconnected unless the bridge actually needs to do work

This keeps the system maintainable and battery-friendly, and avoids the overhead of a separate custom Home Assistant integration.

## Safety Model

This project is intentionally defensive.

- Manual runtime above `3600` seconds is rejected in firmware
- Home Assistant entities are also capped to 1 hour
- Start is only accepted after observed confirmation
- Stop is idempotent and high priority
- No watering command is replayed after reboot or reconnect
- If state is uncertain, the bridge reports `unknown` or `error` instead of guessing

## Current V1 Scope

Supported now:

- Zone 1 start and stop
- Zone 2 start and stop
- Manual duration for both zones
- Hunter battery percentage
- Timer scheduling
- Cycling scheduling
- Passive AMOLED status screen with Wi-Fi, MQTT, Hunter, and health info

Known limitations:

- `FF84` time sync is intentionally not written in v1
- The Hunter clock should be set once with the official app before trusting schedules
- Zone 2 timer and Zone 1 cycling are implemented from protocol symmetry and clearly marked as inferred
- MQTT broker URI should use IPv4 or a normal DNS hostname, not `.local` or IPv6 link-local

## Quick Start

1. Copy `include/bridge_secrets.example.h` to `include/bridge_secrets.h`
2. Fill in Wi-Fi, MQTT, and Hunter MAC settings
3. Build and flash the `waveshare_esp32_c6_touch` PlatformIO environment
4. Add the MQTT integration in Home Assistant
5. Wait for MQTT discovery to create the device and entities
6. Copy the optional Home Assistant UX package/dashboard from `home_assistant/` if you want the simplified mobile control flow

PlatformIO commands:

```powershell
pio run -e waveshare_esp32_c6_touch
pio run -e waveshare_esp32_c6_touch -t upload
pio device monitor -b 115200
```

## Status Display

The onboard AMOLED is intentionally passive.

- It does not trigger extra Hunter BLE connections
- It shows connectivity, Hunter state, and bridge health
- The display sleeps by default to save power
- `PWR` wakes the display briefly
- `BOOT` triggers a restart

## Documentation

- [Architecture](docs/ARCHITECTURE.md)
- [Setup](docs/SETUP.md)
- [Protocol Mapping](docs/PROTOCOL_MAPPING.md)
- [Test Plan](docs/TEST_PLAN.md)
- [Home Assistant UX Layer](home_assistant/README.md)

## Repository Layout

- `src/` firmware implementation
- `include/` public headers and config scaffolding
- `components/esp_lcd_sh8601/` vendored AMOLED panel driver
- `home_assistant/` optional package and dashboard files for the recommended HA UX
- `docs/` architecture, setup, protocol mapping, and test notes
- `test/` protocol fixture validation

## Verification

Useful local checks:

```powershell
python test\verify_protocol_fixtures.py
python -m platformio run -e waveshare_esp32_c6_touch
```

## Project Status

This is a production-minded first version, not a throwaway demo. The BLE protocol handling is based on reverse-engineered captures and test scripts, and the code is structured around transport, protocol, coordination, state, MQTT, and display layers so it can be maintained over time.

# Architecture

## Chosen Architecture

The bridge uses ESP32 firmware plus Home Assistant MQTT discovery.

- ESP32 acts as the only BLE client for the Hunter device.
- Home Assistant talks only to MQTT over the local network.
- The firmware connects to BLE only on demand, performs a bounded session, then disconnects.

This was chosen because it is the simplest fully local option with the least long-term maintenance. A custom Home Assistant integration would add another codebase and another failure surface without helping the BLE reliability problem.

## Firmware Layers

- `BleTransport`
  - Scans for the configured Hunter MAC.
  - Connects with up to 2 attempts.
  - Discovers characteristic handles.
  - Enables notifications on `FF82`, `FF8A`, and `FF8F`.
  - Provides one-operation-at-a-time read/write/wait-for-notify primitives.
- `HunterProtocol`
  - Owns packet builders and validators.
  - Encodes manual prepare, arm, stop, duration, timer blocks, cycling blocks, day masks, and notification decoding.
  - Enforces the 3600 second cap.
- `StateStore`
  - Stores draft settings and last confirmed schedule bytes in NVS.
  - Marks runtime state stale on boot.
  - Never persists “watering is currently active” across reboot.
- `CommandCoordinator`
  - Owns retries, timeouts, state transitions, stop priority, and fail-safe behavior.
  - Keeps draft state separate from confirmed state.
- `MqttBridge`
  - Publishes Home Assistant discovery.
  - Maps MQTT commands to coordinator actions.
  - Publishes entity state, attributes, and inferred/proven origin metadata.

## Data Flow

1. Home Assistant sends an MQTT command to a bridge topic.
2. `MqttBridge` validates/parses the payload.
3. `CommandCoordinator` updates the draft or enqueues a BLE action.
4. `BleTransport` opens a short BLE session.
5. `HunterProtocol` provides the exact bytes written to the Hunter characteristic.
6. Notifications and read-backs are used to confirm success.
7. `StateStore` is updated only with confirmed results.
8. `MqttBridge` republishes new state and attributes to Home Assistant.

## Confirmation And Retries

### Manual start

- Write `FF83` prepare.
- Write `FF86` or `FF8B` duration.
- Wait 500 ms.
- Write `FF83` arm.
- Require an `FF8A` countdown tick within 8 seconds.
- Accept only if the first countdown is within `duration-3 .. duration`.
- If start is not confirmed:
  - send stop,
  - reconnect once,
  - retry the full start flow once,
  - then surface `error` / `unknown`.

### Manual stop

- Write the proven stop packet twice with a 200 ms gap.
- Require `FF82` stop confirmation:
  - `running_flag == 0`
  - payload ends in `80:00`
- If not confirmed:
  - reconnect once,
  - repeat the double-stop sequence once,
  - then fail safe to `error` / `unknown`.

### Schedule apply

- Read current config char.
- Mutate only the mode/day bytes defined by the captures.
- Write config.
- Write block.
- Read both back.
- Accept only on exact byte-for-byte match.

## Fail-Safe Rules

- No duration above 3600 seconds is accepted.
- Stop requests are always treated as high priority.
- No queued commands are replayed after reboot or reconnect.
- Wi-Fi disconnect, MQTT reconnect, BLE timeout, and unexpected disconnect all clear runtime certainty.
- On boot the firmware exposes prior drafts, but runtime state returns to `unknown` until reconfirmed.

## Battery Strategy

- BLE is disconnected by default.
- Every active BLE session refreshes battery once.
- Background battery refresh runs once every 24 hours by default.
- There is no aggressive polling and no permanent BLE subscription.

## Proven Vs Inferred Schedule Paths

- Proven:
  - Zone 1 timer: `FF86` config + `FF87` block
  - Zone 2 cycling: `FF8B` config + `FF8D` block
- Inferred by symmetry:
  - Zone 2 timer: `FF8B` config + `FF8C` block
  - Zone 1 cycling: `FF86` config + `FF88` block

The firmware exposes inferred paths, but Home Assistant attributes flag them as inferred and apply succeeds only if read-back matches exactly.


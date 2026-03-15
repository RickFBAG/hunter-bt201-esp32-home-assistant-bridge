# Test Plan

## Fixture Checks

Run the fixture validator to confirm the checked-in reverse-engineering evidence is internally consistent:

```powershell
python test\verify_protocol_fixtures.py
```

This validates:

- manual prepare/arm/stop packet bytes
- duration packet examples
- proven timer and cycling block payloads
- zone config day-mask mutations from the captured snapshots
- the passcode write example payload for `4931`

## Host-Side Code Review Targets

- `HunterProtocol`
  - reject values above 3600
  - canonicalize days CSV
  - reject invalid times and midnight-crossing cycling windows
- `CommandCoordinator`
  - no non-stop queue buildup
  - stop stays idempotent and bounded
  - no replay after reconnect or reboot
- `MqttBridge`
  - draft updates do not directly imply confirmed schedule success
  - inferred schedule paths expose `origin=inferred`

## On-Device Success Cases

1. Boot bridge and confirm Home Assistant discovery appears.
2. Confirm battery sensor populates.
3. Start Zone 1 for 30 seconds.
4. Confirm `starting -> running`.
5. Stop Zone 1 and confirm `idle`.
6. Repeat 3-5 for Zone 2.
7. Set Zone 1 timer draft, apply, and confirm `applied`.
8. Set Zone 2 cycling draft, apply, and confirm `applied`.
9. Try Zone 2 timer apply and Zone 1 cycling apply, then confirm read-back match before trusting them.

## Failure Cases

1. Try setting manual duration above 3600 and confirm rejection.
2. Power-cycle the ESP32 while it previously believed watering was running and confirm state comes back as `unknown`, not `running`.
3. Disconnect Wi-Fi and confirm runtime certainty is cleared.
4. Turn the Hunter device off or move it out of range during start confirmation and confirm `error` / `unknown`.
5. Send stop repeatedly and confirm no unsafe side effect.
6. Intentionally send malformed day text or time text and confirm the bridge rejects the draft update.
7. Force a read-back mismatch on an inferred schedule path and confirm apply status reports failure.

## Signoff Criteria

- Both zones start and stop from Home Assistant.
- No accepted path exceeds the 1 hour watering limit.
- Battery percentage is visible.
- Start requires confirmation.
- Stop requires confirmation and remains safe to repeat.
- Reboot, BLE drop, Wi-Fi reconnect, and timeout all fail safe.

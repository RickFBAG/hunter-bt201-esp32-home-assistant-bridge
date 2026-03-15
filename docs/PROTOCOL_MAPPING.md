# Home Assistant To BLE Mapping

| Home Assistant control | MQTT command topic suffix | BLE operation | Evidence status |
| --- | --- | --- | --- |
| Zone 1 Start | `cmd/zone1/start` | `FF83` prepare -> `FF86` duration -> delay -> `FF83` arm | Proven |
| Zone 1 Stop | `cmd/zone1/stop` | `FF83` stop twice | Proven |
| Zone 2 Start | `cmd/zone2/start` | `FF83` prepare -> `FF8B` duration -> delay -> `FF83` arm | Proven |
| Zone 2 Stop | `cmd/zone2/stop` | `FF83` stop twice | Proven |
| Zone 1 Manual Duration | `cmd/zone1/manual_duration_seconds/set` | Local validation only until next start | Proven |
| Zone 2 Manual Duration | `cmd/zone2/manual_duration_seconds/set` | Local validation only until next start | Proven |
| Zone 1 Timer Apply | `cmd/zone1/timer/apply` | Read `FF86`, mutate mode/day bytes, write `FF86`, write `FF87`, read back both | Proven |
| Zone 2 Timer Apply | `cmd/zone2/timer/apply` | Read `FF8B`, mutate mode/day bytes, write `FF8B`, write `FF8C`, read back both | Inferred |
| Zone 1 Cycling Apply | `cmd/zone1/cycling/apply` | Read `FF86`, mutate mode/day bytes, write `FF86`, write `FF88`, read back both | Inferred |
| Zone 2 Cycling Apply | `cmd/zone2/cycling/apply` | Read `FF8B`, mutate mode/day bytes, write `FF8B`, write `FF8D`, read back both | Proven |
| Battery Refresh | `cmd/battery/refresh` | Read `2A19` after BLE session open | Proven |

## Characteristic Usage

| Characteristic | Use |
| --- | --- |
| `FF81` | Passcode-related read/write evidence only. Not used as an auth step in v1. |
| `FF82` | Notifications for run/stop state confirmation. |
| `FF83` | Manual command channel. |
| `FF84` | Appears to be time sync. Not written in v1. |
| `FF86` | Zone 1 duration/config bytes and schedule mode/day settings. |
| `FF87` | Zone 1 timer block. |
| `FF88` | Zone 1 cycling block by symmetry. |
| `FF89` | Read-only diagnostic in v1. |
| `FF8A` | Countdown notifications used to validate manual start. |
| `FF8B` | Zone 2 duration/config bytes and schedule mode/day settings. |
| `FF8C` | Zone 2 timer block by symmetry. |
| `FF8D` | Zone 2 cycling block. |
| `FF8E` | Read-only diagnostic in v1. |
| `FF8F` | Notification enabled on connect, logged only in v1. |
| `2A19` | Battery percentage. |

## Config Byte Rules Used In v1

- Config char for Zone 1 is `FF86`.
- Config char for Zone 2 is `FF8B`.
- Byte `0` selects active mode:
  - `0x00` disabled
  - `0x01` timer
  - `0x02` cycling
- Timer days mask uses byte `2`.
- Cycling days mask uses byte `7`.

## Safety Guards

- Manual duration `<= 3600`.
- Timer runtime `<= 3600`.
- Cycling runtime `<= 3600`.
- Cycling window `end - start <= 3600`.
- Start is not accepted without `FF8A` confirmation.
- Stop is not accepted without `FF82` confirmation.


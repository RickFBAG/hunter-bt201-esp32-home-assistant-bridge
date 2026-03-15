import json
from pathlib import Path


ROOT = Path(__file__).resolve().parent
FIXTURES = ROOT / "fixtures" / "hunter_protocol_fixtures.json"


def hx_to_bytes(value: str) -> bytes:
    return bytes(int(part, 16) for part in value.split(":"))


def build_duration(total_seconds: int) -> bytes:
    minutes = total_seconds // 60
    seconds = total_seconds % 60
    return bytes(
        [
            0x04,
            0x01,
            0x7F,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0x00,
            0xFF,
            0xFF,
            0xFF,
            0x00,
            minutes & 0xFF,
            seconds & 0xFF,
            0x00,
            0x1E,
            0x00,
        ]
    )


def assert_equal(label: str, actual, expected) -> None:
    if actual != expected:
        raise AssertionError(f"{label} mismatch:\nactual={actual}\nexpected={expected}")


def main() -> None:
    fixtures = json.loads(FIXTURES.read_text(encoding="utf-8"))

    packets = fixtures["manual_packets"]
    assert_equal("prepare zone1 length", len(hx_to_bytes(packets["prepare_zone1"])), 12)
    assert_equal("arm zone1 length", len(hx_to_bytes(packets["arm_zone1"])), 12)
    assert_equal("prepare zone2 length", len(hx_to_bytes(packets["prepare_zone2"])), 12)
    assert_equal("arm zone2 length", len(hx_to_bytes(packets["arm_zone2"])), 12)
    assert_equal("stop length", len(hx_to_bytes(packets["stop"])), 12)
    assert_equal("stop packet stable", packets["stop"], packets["prepare_zone2"])

    durations = fixtures["duration_packets"]
    assert_equal("duration 60 seconds", build_duration(60), hx_to_bytes(durations["60_seconds"]))
    assert_equal("duration 35 seconds", build_duration(35), hx_to_bytes(durations["35_seconds"]))

    blocks = fixtures["schedule_blocks"]
    assert_equal("zone1 timer block length", len(hx_to_bytes(blocks["zone1_timer_ff87"])), 15)
    assert_equal("zone2 cycling block length", len(hx_to_bytes(blocks["zone2_cycling_ff8d"])), 18)

    snapshots = fixtures["config_snapshots"]
    z1_before = hx_to_bytes(snapshots["zone1_timer_before_ff86"])
    z1_after = hx_to_bytes(snapshots["zone1_timer_after_ff86"])
    assert_equal("zone1 ff86 before length", len(z1_before), 17)
    assert_equal("zone1 ff86 after length", len(z1_after), 17)
    assert_equal("zone1 timer mode byte", z1_after[0], 0x01)
    assert_equal("zone1 timer day byte", z1_after[2], 0x01)

    z2_before = hx_to_bytes(snapshots["zone2_cycling_before_ff8b"])
    z2_after = hx_to_bytes(snapshots["zone2_cycling_after_ff8b"])
    assert_equal("zone2 ff8b before length", len(z2_before), 17)
    assert_equal("zone2 ff8b after length", len(z2_after), 17)
    assert_equal("zone2 cycling mode byte unchanged", z2_after[0], 0x02)
    assert_equal("zone2 cycling day byte", z2_after[7], 0x7F)

    passcode = fixtures["passcode"]
    assert_equal("passcode 4931 bytes", hx_to_bytes(passcode["4931_ff81_write"]), bytes([4, 9, 3, 1]))

    print("All protocol fixture checks passed.")


if __name__ == "__main__":
    main()

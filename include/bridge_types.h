#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace bridge {

constexpr std::size_t kZoneCount = 2;
constexpr std::uint32_t kMaxWateringSeconds = 3600;
constexpr std::uint32_t kStartValidationTimeoutMs = 8000;
constexpr std::uint32_t kStopValidationTimeoutMs = 5000;
constexpr std::uint32_t kGattTimeoutMs = 2000;
constexpr std::uint32_t kConnectTimeoutMs = 20000;
constexpr std::uint32_t kPrepareToArmDelayMs = 500;
constexpr std::uint32_t kStopRepeatDelayMs = 200;
constexpr std::int32_t kOffTimeSeconds = -1;

enum class ZoneId : std::uint8_t {
    Zone1 = 1,
    Zone2 = 2,
};

enum class ZoneRuntimeStatus : std::uint8_t {
    Disconnected = 0,
    Idle,
    Starting,
    Running,
    Stopping,
    Error,
    Unknown,
};

enum class ScheduleMode : std::uint8_t {
    Disabled = 0x00,
    Timer = 0x01,
    Cycling = 0x02,
};

enum class ScheduleOrigin : std::uint8_t {
    Proven = 0,
    Inferred,
};

enum class ApplyTarget : std::uint8_t {
    Timer = 0,
    Cycling,
};

enum class BridgeHealth : std::uint8_t {
    Ok = 0,
    Busy,
    Error,
};

template <std::size_t N>
using FixedString = std::array<char, N>;

struct TimerScheduleDraft {
    bool enabled{false};
    std::uint8_t days_mask{0};
    std::array<std::int32_t, 4> start_times{{
        kOffTimeSeconds, kOffTimeSeconds, kOffTimeSeconds, kOffTimeSeconds,
    }};
    std::uint32_t run_seconds{0};
    ScheduleOrigin origin{ScheduleOrigin::Proven};
};

struct CyclingScheduleDraft {
    bool enabled{false};
    std::uint8_t days_mask{0};
    std::int32_t start1{kOffTimeSeconds};
    std::int32_t end1{kOffTimeSeconds};
    std::int32_t start2{kOffTimeSeconds};
    std::int32_t end2{kOffTimeSeconds};
    std::uint32_t run_seconds{0};
    std::uint32_t soak_seconds{0};
    ScheduleOrigin origin{ScheduleOrigin::Proven};
};

struct ZoneStateRecord {
    std::uint16_t manual_duration_seconds{600};
    TimerScheduleDraft timer{};
    CyclingScheduleDraft cycling{};
    ScheduleMode active_schedule_mode{ScheduleMode::Disabled};
    ZoneRuntimeStatus runtime_status{ZoneRuntimeStatus::Unknown};
    std::uint16_t remaining_seconds{0};
    std::int64_t last_confirmed_epoch_ms{0};
    std::int64_t expected_end_epoch_ms{0};
    bool confirmed_state_stale{true};
    std::array<std::uint8_t, 17> last_config_bytes{};
    std::uint8_t last_config_len{0};
    std::array<std::uint8_t, 15> last_timer_block_bytes{};
    std::uint8_t last_timer_block_len{0};
    std::array<std::uint8_t, 18> last_cycling_block_bytes{};
    std::uint8_t last_cycling_block_len{0};
    FixedString<96> last_error{};
    FixedString<64> last_apply_status{};
};

struct PersistedBridgeState {
    std::uint32_t version{1};
    std::array<ZoneStateRecord, kZoneCount> zones{};
    std::uint8_t battery_percent{0};
    std::int64_t battery_updated_epoch_ms{0};
    FixedString<96> bridge_error{};
};

struct NotificationEvent {
    std::string uuid;
    std::string hex;
    std::array<std::uint8_t, 20> payload{};
    std::size_t payload_len{0};
};

inline constexpr std::size_t to_index(const ZoneId zone) {
    return static_cast<std::size_t>(static_cast<std::uint8_t>(zone) - 1U);
}

inline constexpr ZoneId zone_from_index(const std::size_t index) {
    return index == 0 ? ZoneId::Zone1 : ZoneId::Zone2;
}

const char *to_string(ZoneId zone);
const char *to_string(ZoneRuntimeStatus status);
const char *to_string(ScheduleMode mode);
const char *to_string(ScheduleOrigin origin);
const char *to_string(BridgeHealth health);
void set_fixed_string(FixedString<96> &target, std::string_view value);
void set_fixed_string(FixedString<64> &target, std::string_view value);
std::string fixed_string_to_string(const FixedString<96> &value);
std::string fixed_string_to_string(const FixedString<64> &value);

}  // namespace bridge


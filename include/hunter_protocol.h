#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "bridge_types.h"

namespace bridge::protocol {

inline constexpr char kServiceUuid[] = "0000ff80-0000-1000-8000-00805f9b34fb";
inline constexpr char kBatteryServiceUuid[] = "0000180f-0000-1000-8000-00805f9b34fb";
inline constexpr char kFf81Uuid[] = "0000ff81-0000-1000-8000-00805f9b34fb";
inline constexpr char kFf82Uuid[] = "0000ff82-0000-1000-8000-00805f9b34fb";
inline constexpr char kFf83Uuid[] = "0000ff83-0000-1000-8000-00805f9b34fb";
inline constexpr char kFf84Uuid[] = "0000ff84-0000-1000-8000-00805f9b34fb";
inline constexpr char kFf86Uuid[] = "0000ff86-0000-1000-8000-00805f9b34fb";
inline constexpr char kFf87Uuid[] = "0000ff87-0000-1000-8000-00805f9b34fb";
inline constexpr char kFf88Uuid[] = "0000ff88-0000-1000-8000-00805f9b34fb";
inline constexpr char kFf89Uuid[] = "0000ff89-0000-1000-8000-00805f9b34fb";
inline constexpr char kFf8aUuid[] = "0000ff8a-0000-1000-8000-00805f9b34fb";
inline constexpr char kFf8bUuid[] = "0000ff8b-0000-1000-8000-00805f9b34fb";
inline constexpr char kFf8cUuid[] = "0000ff8c-0000-1000-8000-00805f9b34fb";
inline constexpr char kFf8dUuid[] = "0000ff8d-0000-1000-8000-00805f9b34fb";
inline constexpr char kFf8eUuid[] = "0000ff8e-0000-1000-8000-00805f9b34fb";
inline constexpr char kFf8fUuid[] = "0000ff8f-0000-1000-8000-00805f9b34fb";
inline constexpr char kBatteryLevelUuid[] = "00002a19-0000-1000-8000-00805f9b34fb";

struct ValidationResult {
    bool ok{false};
    std::string error;
};

struct ParsedFf82State {
    std::optional<std::uint8_t> running_flag;
    bool stop_confirmed{false};
};

const char *config_uuid_for_zone(ZoneId zone);
const char *timer_uuid_for_zone(ZoneId zone);
const char *cycling_uuid_for_zone(ZoneId zone);
ScheduleOrigin timer_origin_for_zone(ZoneId zone);
ScheduleOrigin cycling_origin_for_zone(ZoneId zone);

std::array<std::uint8_t, 12> build_prepare_packet(ZoneId zone, std::uint8_t hint = 0x0A);
std::array<std::uint8_t, 12> build_arm_packet(ZoneId zone, std::uint8_t hint = 0x0A);
std::array<std::uint8_t, 12> build_stop_packet();
ValidationResult build_duration_packet(std::uint32_t total_seconds, std::array<std::uint8_t, 17> &out);
ValidationResult build_timer_block(const TimerScheduleDraft &draft, std::array<std::uint8_t, 15> &out);
ValidationResult build_cycling_block(const CyclingScheduleDraft &draft, std::array<std::uint8_t, 18> &out);
ValidationResult mutate_timer_config(
    const std::vector<std::uint8_t> &current,
    bool enabled,
    std::uint8_t days_mask,
    std::array<std::uint8_t, 17> &out);
ValidationResult mutate_cycling_config(
    const std::vector<std::uint8_t> &current,
    bool enabled,
    std::uint8_t days_mask,
    std::array<std::uint8_t, 17> &out);

ValidationResult validate_timer_draft(const TimerScheduleDraft &draft);
ValidationResult validate_cycling_draft(const CyclingScheduleDraft &draft);

std::optional<std::uint32_t> decode_ff8a_remaining_seconds(const std::vector<std::uint8_t> &payload);
ParsedFf82State parse_ff82_state(const std::vector<std::uint8_t> &payload);

ValidationResult parse_time_string(const std::string &text, std::int32_t &seconds_out);
std::string format_time_string(std::int32_t seconds);
ValidationResult parse_days_csv(const std::string &text, std::uint8_t &days_mask_out);
std::string format_days_csv(std::uint8_t mask);

std::string bytes_to_hex(const std::uint8_t *data, std::size_t len);
std::string bytes_to_hex(const std::vector<std::uint8_t> &data);

}  // namespace bridge::protocol


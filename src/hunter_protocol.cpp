#include "hunter_protocol.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <sstream>
#include <string_view>
#include <utility>

namespace bridge::protocol {

namespace {

constexpr std::array<std::pair<std::string_view, std::uint8_t>, 7> kDays{{
    {"MON", 0x01},
    {"TUE", 0x02},
    {"WED", 0x04},
    {"THU", 0x08},
    {"FRI", 0x10},
    {"SAT", 0x20},
    {"SUN", 0x40},
}};

ValidationResult ok() {
    return {true, {}};
}

ValidationResult fail(std::string message) {
    return {false, std::move(message)};
}

bool validate_seconds_of_day(const std::int32_t seconds) {
    return seconds == kOffTimeSeconds || (seconds >= 0 && seconds <= 86399);
}

ValidationResult encode_hms_triplet(std::int32_t seconds, std::uint8_t *target) {
    if (!validate_seconds_of_day(seconds)) {
        return fail("time must be OFF or 00:00:00..23:59:59");
    }
    if (seconds == kOffTimeSeconds) {
        target[0] = 0xFF;
        target[1] = 0xFF;
        target[2] = 0xFF;
        return ok();
    }

    const auto hours = seconds / 3600;
    const auto minutes = (seconds % 3600) / 60;
    const auto secs = seconds % 60;
    target[0] = static_cast<std::uint8_t>(hours);
    target[1] = static_cast<std::uint8_t>(minutes);
    target[2] = static_cast<std::uint8_t>(secs);
    return ok();
}

bool pair_is_partial(const std::int32_t first, const std::int32_t second) {
    return (first == kOffTimeSeconds) != (second == kOffTimeSeconds);
}

}  // namespace

const char *config_uuid_for_zone(const ZoneId zone) {
    return zone == ZoneId::Zone1 ? kFf86Uuid : kFf8bUuid;
}

const char *timer_uuid_for_zone(const ZoneId zone) {
    return zone == ZoneId::Zone1 ? kFf87Uuid : kFf8cUuid;
}

const char *cycling_uuid_for_zone(const ZoneId zone) {
    return zone == ZoneId::Zone1 ? kFf88Uuid : kFf8dUuid;
}

ScheduleOrigin timer_origin_for_zone(const ZoneId zone) {
    return zone == ZoneId::Zone1 ? ScheduleOrigin::Proven : ScheduleOrigin::Inferred;
}

ScheduleOrigin cycling_origin_for_zone(const ZoneId zone) {
    return zone == ZoneId::Zone1 ? ScheduleOrigin::Inferred : ScheduleOrigin::Proven;
}

std::array<std::uint8_t, 12> build_prepare_packet(const ZoneId zone, const std::uint8_t hint) {
    const auto zone_value = static_cast<std::uint8_t>(zone);
    return {0x01, 0x00, zone_value, 0x02, 0x00, zone_value, 0x02, 0x00, hint, 0x00, 0x00, 0x00};
}

std::array<std::uint8_t, 12> build_arm_packet(const ZoneId zone, const std::uint8_t hint) {
    const auto zone_value = static_cast<std::uint8_t>(zone);
    return {0x01, 0x00, zone_value, 0x02, 0x01, zone_value, 0x02, 0x00, hint, 0x00, 0x00, 0x00};
}

std::array<std::uint8_t, 12> build_stop_packet() {
    return {0x01, 0x00, 0x02, 0x02, 0x00, 0x02, 0x02, 0x00, 0x0A, 0x00, 0x00, 0x00};
}

ValidationResult build_duration_packet(const std::uint32_t total_seconds, std::array<std::uint8_t, 17> &out) {
    if (total_seconds > kMaxWateringSeconds) {
        return fail("duration exceeds 3600 seconds");
    }

    const auto minutes = total_seconds / 60U;
    const auto seconds = total_seconds % 60U;
    out = {
        0x04, 0x01, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0x00,
        static_cast<std::uint8_t>(minutes & 0xFFU),
        static_cast<std::uint8_t>(seconds & 0xFFU),
        0x00, 0x1E, 0x00};
    return ok();
}

ValidationResult validate_timer_draft(const TimerScheduleDraft &draft) {
    if (draft.run_seconds > kMaxWateringSeconds) {
        return fail("timer run_seconds exceeds 3600");
    }

    bool has_any_start = false;
    for (const auto start : draft.start_times) {
        if (!validate_seconds_of_day(start)) {
            return fail("timer start must be OFF or 00:00:00..23:59:59");
        }
        has_any_start = has_any_start || start != kOffTimeSeconds;
    }

    if (draft.enabled && !has_any_start) {
        return fail("timer schedule is enabled but no start times are set");
    }

    return ok();
}

ValidationResult validate_cycling_draft(const CyclingScheduleDraft &draft) {
    if (draft.run_seconds > kMaxWateringSeconds) {
        return fail("cycling run_seconds exceeds 3600");
    }
    if (pair_is_partial(draft.start1, draft.end1) || pair_is_partial(draft.start2, draft.end2)) {
        return fail("cycling start/end pairs must both be set or both be OFF");
    }

    const std::array<std::pair<std::int32_t, std::int32_t>, 2> windows{{
        {draft.start1, draft.end1},
        {draft.start2, draft.end2},
    }};

    bool has_any_window = false;
    for (const auto &[start, end] : windows) {
        if (!validate_seconds_of_day(start) || !validate_seconds_of_day(end)) {
            return fail("cycling start/end must be OFF or 00:00:00..23:59:59");
        }
        if (start == kOffTimeSeconds) {
            continue;
        }
        if (end < start) {
            return fail("cycling windows must not cross midnight");
        }
        if (static_cast<std::uint32_t>(end - start) > kMaxWateringSeconds) {
            return fail("cycling window exceeds 3600 seconds");
        }
        has_any_window = true;
    }

    if (draft.enabled && !has_any_window) {
        return fail("cycling schedule is enabled but no windows are set");
    }

    return ok();
}

ValidationResult build_timer_block(const TimerScheduleDraft &draft, std::array<std::uint8_t, 15> &out) {
    const auto validation = validate_timer_draft(draft);
    if (!validation.ok) {
        return validation;
    }

    std::size_t offset = 0;
    for (const auto start : draft.start_times) {
        auto result = encode_hms_triplet(start, out.data() + offset);
        if (!result.ok) {
            return result;
        }
        offset += 3;
    }

    auto run_triplet = encode_hms_triplet(static_cast<std::int32_t>(draft.run_seconds), out.data() + 12U);
    if (!run_triplet.ok) {
        return run_triplet;
    }
    return ok();
}

ValidationResult build_cycling_block(const CyclingScheduleDraft &draft, std::array<std::uint8_t, 18> &out) {
    const auto validation = validate_cycling_draft(draft);
    if (!validation.ok) {
        return validation;
    }

    const std::array<std::int32_t, 6> parts{{
        draft.start1,
        draft.end1,
        draft.start2,
        draft.end2,
        static_cast<std::int32_t>(draft.run_seconds),
        static_cast<std::int32_t>(draft.soak_seconds),
    }};

    std::size_t offset = 0;
    for (const auto seconds : parts) {
        auto result = encode_hms_triplet(seconds, out.data() + offset);
        if (!result.ok) {
            return result;
        }
        offset += 3;
    }

    return ok();
}

ValidationResult mutate_timer_config(
    const std::vector<std::uint8_t> &current,
    const bool enabled,
    const std::uint8_t days_mask,
    std::array<std::uint8_t, 17> &out) {
    if (current.size() != out.size()) {
        return fail("timer config length must be 17 bytes");
    }
    std::copy(current.begin(), current.end(), out.begin());
    out[0] = enabled ? static_cast<std::uint8_t>(ScheduleMode::Timer) : 0x00;
    out[2] = days_mask;
    return ok();
}

ValidationResult mutate_cycling_config(
    const std::vector<std::uint8_t> &current,
    const bool enabled,
    const std::uint8_t days_mask,
    std::array<std::uint8_t, 17> &out) {
    if (current.size() != out.size()) {
        return fail("cycling config length must be 17 bytes");
    }
    std::copy(current.begin(), current.end(), out.begin());
    out[0] = enabled ? static_cast<std::uint8_t>(ScheduleMode::Cycling) : 0x00;
    out[7] = days_mask;
    return ok();
}

std::optional<std::uint32_t> decode_ff8a_remaining_seconds(const std::vector<std::uint8_t> &payload) {
    if (payload.size() != 16U) {
        return std::nullopt;
    }
    const auto minutes = payload[11];
    const auto seconds = payload[12];
    if (seconds > 59U) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(minutes) * 60U + static_cast<std::uint32_t>(seconds);
}

ParsedFf82State parse_ff82_state(const std::vector<std::uint8_t> &payload) {
    ParsedFf82State parsed{};
    if (payload.size() >= 5U) {
        parsed.running_flag = payload[4];
    }
    if (payload.size() >= 2U && payload[payload.size() - 2U] == 0x80U && payload[payload.size() - 1U] == 0x00U) {
        parsed.stop_confirmed = parsed.running_flag.has_value() && parsed.running_flag.value() == 0U;
    }
    return parsed;
}

ValidationResult parse_time_string(const std::string &text, std::int32_t &seconds_out) {
    if (text == "OFF") {
        seconds_out = kOffTimeSeconds;
        return ok();
    }

    int hours = 0;
    int minutes = 0;
    int seconds = 0;
    if (std::sscanf(text.c_str(), "%d:%d:%d", &hours, &minutes, &seconds) != 3) {
        return fail("time must be OFF or HH:MM:SS");
    }
    if (hours < 0 || hours > 23 || minutes < 0 || minutes > 59 || seconds < 0 || seconds > 59) {
        return fail("time must be within 00:00:00..23:59:59");
    }
    seconds_out = hours * 3600 + minutes * 60 + seconds;
    return ok();
}

std::string format_time_string(const std::int32_t seconds) {
    if (seconds == kOffTimeSeconds) {
        return "OFF";
    }

    char buffer[16];
    const auto hours = seconds / 3600;
    const auto minutes = (seconds % 3600) / 60;
    const auto secs = seconds % 60;
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", hours, minutes, secs);
    return buffer;
}

ValidationResult parse_days_csv(const std::string &text, std::uint8_t &days_mask_out) {
    std::string normalized;
    normalized.reserve(text.size());
    for (char ch : text) {
        normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }

    if (normalized == "ALL") {
        days_mask_out = 0x7F;
        return ok();
    }
    if (normalized == "NONE" || normalized.empty()) {
        days_mask_out = 0x00;
        return ok();
    }

    std::uint8_t mask = 0;
    std::stringstream stream(normalized);
    std::string token;
    while (std::getline(stream, token, ',')) {
        token.erase(std::remove_if(token.begin(), token.end(), [](unsigned char ch) { return std::isspace(ch) != 0; }), token.end());
        if (token.empty()) {
            continue;
        }

        bool found = false;
        for (const auto &[name, bit] : kDays) {
            if (token == name) {
                mask |= bit;
                found = true;
                break;
            }
        }
        if (!found) {
            return fail("days must be MON..SUN, ALL, or NONE");
        }
    }

    days_mask_out = mask;
    return ok();
}

std::string format_days_csv(const std::uint8_t mask) {
    if (mask == 0x00U) {
        return "NONE";
    }
    if (mask == 0x7FU) {
        return "ALL";
    }

    std::string result;
    for (const auto &[name, bit] : kDays) {
        if ((mask & bit) == 0U) {
            continue;
        }
        if (!result.empty()) {
            result += ",";
        }
        result += name;
    }
    return result.empty() ? "NONE" : result;
}

std::string bytes_to_hex(const std::uint8_t *data, const std::size_t len) {
    std::ostringstream stream;
    stream.setf(std::ios::hex, std::ios::basefield);
    stream.fill('0');
    for (std::size_t index = 0; index < len; ++index) {
        stream.width(2);
        stream << static_cast<int>(data[index]);
        if (index + 1U < len) {
            stream << ":";
        }
    }
    return stream.str();
}

std::string bytes_to_hex(const std::vector<std::uint8_t> &data) {
    return bytes_to_hex(data.data(), data.size());
}

}  // namespace bridge::protocol

#include "bridge_types.h"

#include <algorithm>

namespace bridge {

namespace {

template <std::size_t N>
void copy_fixed(FixedString<N> &target, std::string_view value) {
    target.fill('\0');
    const auto size = std::min(value.size(), target.size() - 1U);
    std::copy_n(value.data(), size, target.data());
}

template <std::size_t N>
std::string to_std_string(const FixedString<N> &value) {
    return std::string(value.data());
}

}  // namespace

const char *to_string(const ZoneId zone) {
    return zone == ZoneId::Zone1 ? "zone1" : "zone2";
}

const char *to_string(const ZoneRuntimeStatus status) {
    switch (status) {
        case ZoneRuntimeStatus::Disconnected:
            return "disconnected";
        case ZoneRuntimeStatus::Idle:
            return "idle";
        case ZoneRuntimeStatus::Starting:
            return "starting";
        case ZoneRuntimeStatus::Running:
            return "running";
        case ZoneRuntimeStatus::Stopping:
            return "stopping";
        case ZoneRuntimeStatus::Error:
            return "error";
        case ZoneRuntimeStatus::Unknown:
        default:
            return "unknown";
    }
}

const char *to_string(const ScheduleMode mode) {
    switch (mode) {
        case ScheduleMode::Timer:
            return "timer";
        case ScheduleMode::Cycling:
            return "cycling";
        case ScheduleMode::Disabled:
        default:
            return "disabled";
    }
}

const char *to_string(const ScheduleOrigin origin) {
    return origin == ScheduleOrigin::Inferred ? "inferred" : "proven";
}

const char *to_string(const BridgeHealth health) {
    switch (health) {
        case BridgeHealth::Busy:
            return "busy";
        case BridgeHealth::Error:
            return "error";
        case BridgeHealth::Ok:
        default:
            return "ok";
    }
}

void set_fixed_string(FixedString<96> &target, std::string_view value) {
    copy_fixed(target, value);
}

void set_fixed_string(FixedString<64> &target, std::string_view value) {
    copy_fixed(target, value);
}

std::string fixed_string_to_string(const FixedString<96> &value) {
    return to_std_string(value);
}

std::string fixed_string_to_string(const FixedString<64> &value) {
    return to_std_string(value);
}

}  // namespace bridge


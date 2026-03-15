#include "state_store.h"

#include <cstring>

#include "bridge_types.h"
#include "hunter_protocol.h"
#include "nvs_flash.h"

namespace bridge {

namespace {

void reset_zone_defaults(ZoneStateRecord &zone, const ZoneId zone_id) {
    zone = ZoneStateRecord{};
    zone.manual_duration_seconds = 600;
    zone.timer.origin = protocol::timer_origin_for_zone(zone_id);
    zone.cycling.origin = protocol::cycling_origin_for_zone(zone_id);
    set_fixed_string(zone.last_apply_status, "not_applied");
}

}  // namespace

StateStore::~StateStore() {
    if (nvs_handle_ != 0) {
        nvs_close(nvs_handle_);
        nvs_handle_ = 0;
    }
}

esp_err_t StateStore::init() {
    const auto nvs_status = nvs_flash_init();
    if (nvs_status != ESP_OK && nvs_status != ESP_ERR_NVS_NO_FREE_PAGES && nvs_status != ESP_ERR_NVS_NEW_VERSION_FOUND) {
        return nvs_status;
    }
    if (nvs_status == ESP_ERR_NVS_NO_FREE_PAGES || nvs_status == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    const auto open_status = nvs_open(kNamespace, NVS_READWRITE, &nvs_handle_);
    if (open_status != ESP_OK) {
        return open_status;
    }

    size_t required_size = sizeof(state_);
    const auto get_status = nvs_get_blob(nvs_handle_, kStateKey, &state_, &required_size);
    if (get_status == ESP_ERR_NVS_NOT_FOUND || required_size != sizeof(state_) || state_.version != 1U) {
        reset_defaults();
        return save();
    }
    if (get_status != ESP_OK) {
        return get_status;
    }

    state_.zones[0].timer.origin = protocol::timer_origin_for_zone(ZoneId::Zone1);
    state_.zones[0].cycling.origin = protocol::cycling_origin_for_zone(ZoneId::Zone1);
    state_.zones[1].timer.origin = protocol::timer_origin_for_zone(ZoneId::Zone2);
    state_.zones[1].cycling.origin = protocol::cycling_origin_for_zone(ZoneId::Zone2);
    return ESP_OK;
}

PersistedBridgeState &StateStore::state() {
    return state_;
}

const PersistedBridgeState &StateStore::state() const {
    return state_;
}

ZoneStateRecord &StateStore::zone(const ZoneId zone_id) {
    return state_.zones[to_index(zone_id)];
}

const ZoneStateRecord &StateStore::zone(const ZoneId zone_id) const {
    return state_.zones[to_index(zone_id)];
}

void StateStore::reset_defaults() {
    state_ = PersistedBridgeState{};
    state_.version = 1;
    reset_zone_defaults(state_.zones[0], ZoneId::Zone1);
    reset_zone_defaults(state_.zones[1], ZoneId::Zone2);
    clear_bridge_error();
}

void StateStore::mark_boot_stale() {
    for (std::size_t index = 0; index < kZoneCount; ++index) {
        auto &zone_state = state_.zones[index];
        zone_state.runtime_status = ZoneRuntimeStatus::Unknown;
        zone_state.remaining_seconds = 0;
        zone_state.expected_end_epoch_ms = 0;
        zone_state.confirmed_state_stale = true;
        set_fixed_string(zone_state.last_error, "boot requires reconfirmation");
    }
}

void StateStore::mark_all_unknown(const std::string_view reason, const bool error_state) {
    for (std::size_t index = 0; index < kZoneCount; ++index) {
        auto &zone_state = state_.zones[index];
        zone_state.runtime_status = error_state ? ZoneRuntimeStatus::Error : ZoneRuntimeStatus::Unknown;
        zone_state.remaining_seconds = 0;
        zone_state.expected_end_epoch_ms = 0;
        zone_state.confirmed_state_stale = true;
        set_fixed_string(zone_state.last_error, reason);
    }
}

void StateStore::set_bridge_error(const std::string_view error) {
    set_fixed_string(state_.bridge_error, error);
}

void StateStore::set_zone_error(const ZoneId zone_id, const std::string_view error) {
    auto &zone_state = zone(zone_id);
    zone_state.runtime_status = ZoneRuntimeStatus::Error;
    zone_state.confirmed_state_stale = true;
    set_fixed_string(zone_state.last_error, error);
}

void StateStore::set_apply_status(const ZoneId zone_id, const std::string_view status) {
    set_fixed_string(zone(zone_id).last_apply_status, status);
}

void StateStore::clear_zone_error(const ZoneId zone_id) {
    set_fixed_string(zone(zone_id).last_error, "");
}

void StateStore::clear_bridge_error() {
    set_fixed_string(state_.bridge_error, "");
}

esp_err_t StateStore::save() {
    if (nvs_handle_ == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    const auto set_status = nvs_set_blob(nvs_handle_, kStateKey, &state_, sizeof(state_));
    if (set_status != ESP_OK) {
        return set_status;
    }
    return nvs_commit(nvs_handle_);
}

}  // namespace bridge

#include "command_coordinator.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>
#include <vector>

extern "C" {
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
}

#include "hunter_protocol.h"

namespace bridge {

namespace {

constexpr char kTag[] = "Coordinator";

bool payload_equals(const std::vector<std::uint8_t> &payload, const std::uint8_t *expected, const std::size_t len) {
    return payload.size() == len && std::equal(payload.begin(), payload.end(), expected);
}

}  // namespace

CommandCoordinator::CommandCoordinator(BleTransport &transport, StateStore &state_store)
    : transport_(transport), state_store_(state_store) {}

CommandCoordinator::~CommandCoordinator() {
    if (remaining_timer_ != nullptr) {
        esp_timer_stop(remaining_timer_);
        esp_timer_delete(remaining_timer_);
        remaining_timer_ = nullptr;
    }
    if (telemetry_mutex_ != nullptr) {
        vSemaphoreDelete(telemetry_mutex_);
        telemetry_mutex_ = nullptr;
    }
}

esp_err_t CommandCoordinator::init() {
    queue_ = xQueueCreate(8, sizeof(Command));
    if (queue_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    telemetry_mutex_ = xSemaphoreCreateMutex();
    if (telemetry_mutex_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    esp_timer_create_args_t timer_args = {};
    timer_args.callback = &CommandCoordinator::remaining_timer_cb;
    timer_args.arg = this;
    timer_args.dispatch_method = ESP_TIMER_TASK;
    timer_args.name = "remaining";
    timer_args.skip_unhandled_events = true;
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &remaining_timer_));

    transport_.set_disconnect_callback([this](const int reason) { on_transport_disconnect(reason); });
    const auto created = xTaskCreate(
        &CommandCoordinator::worker_task,
        "bridge_worker",
        10240,
        this,
        5,
        &task_handle_);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void CommandCoordinator::set_event_sink(BridgeEventSink *sink) {
    sink_ = sink;
}

bool CommandCoordinator::request_start(const ZoneId zone) {
    if (busy_.load() || uxQueueMessagesWaiting(static_cast<QueueHandle_t>(queue_)) > 0) {
        return false;
    }

    auto &zone_state = state_store_.zone(zone);
    zone_state.runtime_status = ZoneRuntimeStatus::Starting;
    zone_state.confirmed_state_stale = true;
    state_store_.clear_zone_error(zone);
    notify_sink();
    return enqueue_command({CommandType::Start, zone}, false);
}

bool CommandCoordinator::request_stop(const ZoneId zone) {
    urgent_stop_requested_.store(true);
    for (std::size_t index = 0; index < kZoneCount; ++index) {
        if (state_store_.state().zones[index].runtime_status == ZoneRuntimeStatus::Running ||
            state_store_.state().zones[index].runtime_status == ZoneRuntimeStatus::Starting) {
            state_store_.state().zones[index].runtime_status = ZoneRuntimeStatus::Stopping;
        }
    }
    notify_sink();
    return enqueue_command({CommandType::Stop, zone}, true);
}

bool CommandCoordinator::request_apply_schedule(const ZoneId zone, const ApplyTarget target) {
    if (busy_.load() || uxQueueMessagesWaiting(static_cast<QueueHandle_t>(queue_)) > 0) {
        return false;
    }
    return enqueue_command({target == ApplyTarget::Timer ? CommandType::ApplyTimer : CommandType::ApplyCycling, zone}, false);
}

bool CommandCoordinator::request_refresh_battery() {
    if (busy_.load() || uxQueueMessagesWaiting(static_cast<QueueHandle_t>(queue_)) > 0) {
        return false;
    }
    return enqueue_command({CommandType::RefreshBattery, ZoneId::Zone1}, false);
}

bool CommandCoordinator::update_manual_duration(const ZoneId zone, const std::uint32_t seconds, std::string &error) {
    if (seconds > kMaxWateringSeconds) {
        error = "manual duration exceeds 3600 seconds";
        return false;
    }
    state_store_.zone(zone).manual_duration_seconds = static_cast<std::uint16_t>(seconds);
    state_store_.save();
    notify_sink();
    return true;
}

bool CommandCoordinator::update_timer_draft(const ZoneId zone, const TimerScheduleDraft &draft, std::string &error) {
    const auto validation = protocol::validate_timer_draft(draft);
    if (!validation.ok) {
        error = validation.error;
        return false;
    }
    auto &zone_state = state_store_.zone(zone);
    zone_state.timer = draft;
    zone_state.timer.origin = protocol::timer_origin_for_zone(zone);
    state_store_.save();
    notify_sink();
    return true;
}

bool CommandCoordinator::update_cycling_draft(const ZoneId zone, const CyclingScheduleDraft &draft, std::string &error) {
    const auto validation = protocol::validate_cycling_draft(draft);
    if (!validation.ok) {
        error = validation.error;
        return false;
    }
    auto &zone_state = state_store_.zone(zone);
    zone_state.cycling = draft;
    zone_state.cycling.origin = protocol::cycling_origin_for_zone(zone);
    state_store_.save();
    notify_sink();
    return true;
}

void CommandCoordinator::fail_safe_reset(const std::string_view reason, const bool error_state) {
    urgent_stop_requested_.store(false);
    state_store_.mark_all_unknown(reason, error_state);
    state_store_.set_bridge_error(reason);
    state_store_.save();
    ensure_remaining_timer_state();
    notify_sink();
}

const PersistedBridgeState &CommandCoordinator::state() const {
    return state_store_.state();
}

bool CommandCoordinator::is_busy() const {
    return busy_.load();
}

BleTelemetrySnapshot CommandCoordinator::ble_telemetry() const {
    BleTelemetrySnapshot snapshot{};
    if (telemetry_mutex_ != nullptr) {
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(telemetry_mutex_), portMAX_DELAY);
        snapshot.last_attempt_ms = last_ble_attempt_ms_;
        snapshot.last_success_ms = last_ble_success_ms_;
        snapshot.last_status = last_ble_status_;
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(telemetry_mutex_));
    }
    return snapshot;
}

bool CommandCoordinator::enqueue_command(const Command command, const bool high_priority) {
    BaseType_t result = pdFALSE;
    if (high_priority) {
        result = xQueueSendToFront(static_cast<QueueHandle_t>(queue_), &command, 0);
    } else {
        result = xQueueSend(static_cast<QueueHandle_t>(queue_), &command, 0);
    }
    return result == pdTRUE;
}

void CommandCoordinator::worker_task(void *param) {
    static_cast<CommandCoordinator *>(param)->task_loop();
}

void CommandCoordinator::remaining_timer_cb(void *arg) {
    auto *self = static_cast<CommandCoordinator *>(arg);
    self->refresh_remaining_counts();
}

void CommandCoordinator::task_loop() {
    Command command{};
    while (true) {
        if (xQueueReceive(static_cast<QueueHandle_t>(queue_), &command, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        busy_.store(true);
        bool success = false;
        switch (command.type) {
            case CommandType::Start:
                success = perform_start(command.zone);
                break;
            case CommandType::Stop:
                success = perform_stop(command.zone);
                break;
            case CommandType::ApplyTimer:
                success = perform_apply_schedule(command.zone, ApplyTarget::Timer);
                break;
            case CommandType::ApplyCycling:
                success = perform_apply_schedule(command.zone, ApplyTarget::Cycling);
                break;
            case CommandType::RefreshBattery:
                success = perform_refresh_battery();
                break;
        }

        if (success) {
            state_store_.clear_bridge_error();
        } else if (fixed_string_to_string(state_store_.state().bridge_error).empty()) {
            state_store_.set_bridge_error("command_failed");
        }

        state_store_.save();
        ensure_remaining_timer_state();
        notify_sink();
        busy_.store(false);
    }
}

void CommandCoordinator::notify_sink() {
    if (sink_ != nullptr) {
        sink_->on_state_changed();
    }
}

std::int64_t CommandCoordinator::now_ms() const {
    return esp_timer_get_time() / 1000;
}

void CommandCoordinator::on_transport_disconnect(const int reason) {
    transport_drop_detected_.store(true);
    set_ble_status("ble_disconnect", false);
    fail_safe_reset("ble_disconnect", true);
    ESP_LOGW(kTag, "transport disconnect reason=%d", reason);
}

bool CommandCoordinator::open_session() {
    transport_drop_detected_.store(false);
    if (telemetry_mutex_ != nullptr) {
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(telemetry_mutex_), portMAX_DELAY);
        last_ble_attempt_ms_ = now_ms();
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(telemetry_mutex_));
    }
    if (!transport_.connect()) {
        set_ble_status(transport_.last_error(), false);
        state_store_.set_bridge_error(transport_.last_error());
        return false;
    }
    set_ble_status("connected", true);
    update_battery_from_session();
    return true;
}

void CommandCoordinator::close_session() {
    transport_.disconnect();
}

void CommandCoordinator::update_battery_from_session() {
    std::uint8_t battery = 0;
    if (transport_.read_battery_percent(battery)) {
        state_store_.state().battery_percent = battery;
        state_store_.state().battery_updated_epoch_ms = now_ms();
        set_ble_status("battery_read_ok", true);
    } else {
        set_ble_status("battery_read_failed", false);
    }
}

void CommandCoordinator::set_ble_status(const std::string_view status, const bool connected_successfully) {
    if (telemetry_mutex_ == nullptr) {
        return;
    }

    xSemaphoreTake(static_cast<SemaphoreHandle_t>(telemetry_mutex_), portMAX_DELAY);
    set_fixed_string(last_ble_status_, status);
    if (connected_successfully) {
        last_ble_success_ms_ = now_ms();
    }
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(telemetry_mutex_));
}

bool CommandCoordinator::perform_start(const ZoneId zone) {
    const auto requested_seconds = state_store_.zone(zone).manual_duration_seconds;
    if (requested_seconds > kMaxWateringSeconds) {
        state_store_.set_zone_error(zone, "duration_above_3600");
        return false;
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!open_session()) {
            continue;
        }
        const bool success = perform_start_attempt(zone, requested_seconds);
        close_session();
        if (success) {
            set_ble_status("start_confirmed", true);
            return true;
        }
        perform_stop(zone);
    }

    state_store_.zone(zone).runtime_status = ZoneRuntimeStatus::Unknown;
    state_store_.zone(zone).confirmed_state_stale = true;
    state_store_.set_zone_error(zone, "start_confirmation_failed");
    set_ble_status("start_confirmation_failed", false);
    return false;
}

bool CommandCoordinator::perform_start_attempt(const ZoneId zone, const std::uint32_t requested_seconds) {
    std::array<std::uint8_t, 17> duration_packet{};
    const auto duration_result = protocol::build_duration_packet(requested_seconds, duration_packet);
    if (!duration_result.ok) {
        state_store_.set_zone_error(zone, duration_result.error);
        return false;
    }

    transport_.clear_notifications();
    const auto prepare = protocol::build_prepare_packet(zone);
    const auto arm = protocol::build_arm_packet(zone);
    if (!transport_.write_characteristic(protocol::kFf83Uuid, prepare.data(), prepare.size())) {
        return false;
    }
    if (!transport_.write_characteristic(protocol::config_uuid_for_zone(zone), duration_packet.data(), duration_packet.size())) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(kPrepareToArmDelayMs));

    if (urgent_stop_requested_.load()) {
        return perform_stop_attempt();
    }

    if (!transport_.write_characteristic(protocol::kFf83Uuid, arm.data(), arm.size())) {
        return false;
    }

    const auto start_deadline = now_ms() + kStartValidationTimeoutMs;
    bool saw_secondary_running = false;
    while (now_ms() < start_deadline) {
        if (urgent_stop_requested_.load()) {
            return perform_stop_attempt();
        }

        NotificationEvent event{};
        if (!transport_.wait_for_notification(event, 250)) {
            continue;
        }

        std::vector<std::uint8_t> payload(event.payload.begin(), event.payload.begin() + event.payload_len);
        if (event.uuid == protocol::kFf8aUuid) {
            const auto remaining = protocol::decode_ff8a_remaining_seconds(payload);
            if (!remaining.has_value()) {
                continue;
            }
            const auto low = requested_seconds > 3 ? requested_seconds - 3 : 0;
            if (remaining.value() >= low && remaining.value() <= requested_seconds) {
                mark_running(zone, remaining.value());
                return true;
            }
        } else if (event.uuid == protocol::kFf82Uuid) {
            const auto ff82 = protocol::parse_ff82_state(payload);
            saw_secondary_running = ff82.running_flag.has_value() && ff82.running_flag.value() == 1;
        }
    }

    if (saw_secondary_running) {
        state_store_.set_zone_error(zone, "ff82_running_without_ff8a");
    }
    return false;
}

bool CommandCoordinator::perform_stop(const ZoneId requested_zone) {
    (void)requested_zone;
    urgent_stop_requested_.store(false);
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!open_session()) {
            continue;
        }
        const bool success = perform_stop_attempt();
        close_session();
        if (success) {
            mark_idle_all("stop_confirmed");
            set_ble_status("stop_confirmed", true);
            return true;
        }
    }

    set_ble_status("stop_confirmation_failed", false);
    fail_safe_reset("stop_confirmation_failed", true);
    return false;
}

bool CommandCoordinator::perform_stop_attempt() {
    transport_.clear_notifications();
    const auto stop_packet = protocol::build_stop_packet();
    if (!transport_.write_characteristic(protocol::kFf83Uuid, stop_packet.data(), stop_packet.size())) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(kStopRepeatDelayMs));
    if (!transport_.write_characteristic(protocol::kFf83Uuid, stop_packet.data(), stop_packet.size())) {
        return false;
    }

    const auto deadline = now_ms() + kStopValidationTimeoutMs;
    while (now_ms() < deadline) {
        NotificationEvent event{};
        if (!transport_.wait_for_notification(event, 250)) {
            continue;
        }
        if (event.uuid != protocol::kFf82Uuid) {
            continue;
        }
        std::vector<std::uint8_t> payload(event.payload.begin(), event.payload.begin() + event.payload_len);
        const auto parsed = protocol::parse_ff82_state(payload);
        if (parsed.stop_confirmed) {
            return true;
        }
    }
    return false;
}

bool CommandCoordinator::perform_apply_schedule(const ZoneId zone, const ApplyTarget target) {
    if (!open_session()) {
        state_store_.set_apply_status(zone, "session_open_failed");
        return false;
    }

    auto &zone_state = state_store_.zone(zone);
    std::vector<std::uint8_t> current_config;
    if (!transport_.read_characteristic(protocol::config_uuid_for_zone(zone), current_config)) {
        state_store_.set_apply_status(zone, "config_read_failed");
        close_session();
        return false;
    }

    std::array<std::uint8_t, 17> config_bytes{};
    std::vector<std::uint8_t> readback_config;
    std::vector<std::uint8_t> readback_block;
    bool success = false;

    if (target == ApplyTarget::Timer) {
        std::array<std::uint8_t, 15> block{};
        const auto block_result = protocol::build_timer_block(zone_state.timer, block);
        const auto config_result = protocol::mutate_timer_config(current_config, zone_state.timer.enabled, zone_state.timer.days_mask, config_bytes);
        if (!block_result.ok || !config_result.ok) {
            state_store_.set_apply_status(zone, "timer_validation_failed");
            close_session();
            return false;
        }

        success =
            transport_.write_characteristic(protocol::config_uuid_for_zone(zone), config_bytes.data(), config_bytes.size()) &&
            transport_.write_characteristic(protocol::timer_uuid_for_zone(zone), block.data(), block.size()) &&
            transport_.read_characteristic(protocol::config_uuid_for_zone(zone), readback_config) &&
            transport_.read_characteristic(protocol::timer_uuid_for_zone(zone), readback_block) &&
            payload_equals(readback_config, config_bytes.data(), config_bytes.size()) &&
            payload_equals(readback_block, block.data(), block.size());

        if (success) {
            zone_state.active_schedule_mode = zone_state.timer.enabled ? ScheduleMode::Timer : ScheduleMode::Disabled;
            zone_state.last_config_len = config_bytes.size();
            zone_state.last_timer_block_len = block.size();
            std::copy(config_bytes.begin(), config_bytes.end(), zone_state.last_config_bytes.begin());
            std::copy(block.begin(), block.end(), zone_state.last_timer_block_bytes.begin());
            zone_state.confirmed_state_stale = false;
            state_store_.set_apply_status(zone, "applied");
            set_ble_status("timer_applied", true);
        } else {
            state_store_.set_apply_status(zone, "readback_mismatch");
            set_ble_status("timer_readback_mismatch", false);
        }
    } else {
        std::array<std::uint8_t, 18> block{};
        const auto block_result = protocol::build_cycling_block(zone_state.cycling, block);
        const auto config_result = protocol::mutate_cycling_config(current_config, zone_state.cycling.enabled, zone_state.cycling.days_mask, config_bytes);
        if (!block_result.ok || !config_result.ok) {
            state_store_.set_apply_status(zone, "cycling_validation_failed");
            close_session();
            return false;
        }

        success =
            transport_.write_characteristic(protocol::config_uuid_for_zone(zone), config_bytes.data(), config_bytes.size()) &&
            transport_.write_characteristic(protocol::cycling_uuid_for_zone(zone), block.data(), block.size()) &&
            transport_.read_characteristic(protocol::config_uuid_for_zone(zone), readback_config) &&
            transport_.read_characteristic(protocol::cycling_uuid_for_zone(zone), readback_block) &&
            payload_equals(readback_config, config_bytes.data(), config_bytes.size()) &&
            payload_equals(readback_block, block.data(), block.size());

        if (success) {
            zone_state.active_schedule_mode = zone_state.cycling.enabled ? ScheduleMode::Cycling : ScheduleMode::Disabled;
            zone_state.last_config_len = config_bytes.size();
            zone_state.last_cycling_block_len = block.size();
            std::copy(config_bytes.begin(), config_bytes.end(), zone_state.last_config_bytes.begin());
            std::copy(block.begin(), block.end(), zone_state.last_cycling_block_bytes.begin());
            zone_state.confirmed_state_stale = false;
            state_store_.set_apply_status(zone, "applied");
            set_ble_status("cycling_applied", true);
        } else {
            state_store_.set_apply_status(zone, "readback_mismatch");
            set_ble_status("cycling_readback_mismatch", false);
        }
    }

    close_session();
    return success;
}

bool CommandCoordinator::perform_refresh_battery() {
    if (!open_session()) {
        set_ble_status("battery_session_failed", false);
        return false;
    }
    close_session();
    set_ble_status("battery_refresh_ok", true);
    return true;
}

void CommandCoordinator::mark_running(const ZoneId zone, const std::uint32_t remaining_seconds) {
    for (std::size_t index = 0; index < kZoneCount; ++index) {
        auto &zone_state = state_store_.state().zones[index];
        zone_state.remaining_seconds = 0;
        zone_state.expected_end_epoch_ms = 0;
        zone_state.runtime_status = ZoneRuntimeStatus::Idle;
    }

    auto &zone_state = state_store_.zone(zone);
    zone_state.runtime_status = ZoneRuntimeStatus::Running;
    zone_state.remaining_seconds = static_cast<std::uint16_t>(remaining_seconds);
    zone_state.last_confirmed_epoch_ms = now_ms();
    zone_state.expected_end_epoch_ms = now_ms() + static_cast<std::int64_t>(remaining_seconds) * 1000;
    zone_state.confirmed_state_stale = false;
    state_store_.clear_zone_error(zone);
}

void CommandCoordinator::mark_idle_all(const std::string_view reason) {
    (void)reason;
    for (std::size_t index = 0; index < kZoneCount; ++index) {
        auto &zone_state = state_store_.state().zones[index];
        zone_state.runtime_status = ZoneRuntimeStatus::Idle;
        zone_state.remaining_seconds = 0;
        zone_state.expected_end_epoch_ms = 0;
        zone_state.last_confirmed_epoch_ms = now_ms();
        zone_state.confirmed_state_stale = false;
        set_fixed_string(zone_state.last_error, "");
    }
}

void CommandCoordinator::mark_zone_status(const ZoneId zone, const ZoneRuntimeStatus status, const std::string_view error) {
    auto &zone_state = state_store_.zone(zone);
    zone_state.runtime_status = status;
    if (!error.empty()) {
        set_fixed_string(zone_state.last_error, error);
    }
}

void CommandCoordinator::refresh_remaining_counts() {
    bool changed = false;
    const auto current = now_ms();
    for (std::size_t index = 0; index < kZoneCount; ++index) {
        auto &zone_state = state_store_.state().zones[index];
        if (zone_state.runtime_status != ZoneRuntimeStatus::Running) {
            continue;
        }
        if (zone_state.expected_end_epoch_ms <= current) {
            zone_state.runtime_status = ZoneRuntimeStatus::Idle;
            zone_state.remaining_seconds = 0;
            zone_state.expected_end_epoch_ms = 0;
            changed = true;
            continue;
        }
        const auto seconds_left = static_cast<std::uint16_t>((zone_state.expected_end_epoch_ms - current + 999) / 1000);
        if (zone_state.remaining_seconds != seconds_left) {
            zone_state.remaining_seconds = seconds_left;
            changed = true;
        }
    }

    ensure_remaining_timer_state();
    if (changed) {
        notify_sink();
    }
}

void CommandCoordinator::ensure_remaining_timer_state() {
    bool any_running = false;
    for (const auto &zone_state : state_store_.state().zones) {
        any_running = any_running || zone_state.runtime_status == ZoneRuntimeStatus::Running;
    }
    if (any_running) {
        if (!esp_timer_is_active(remaining_timer_)) {
            esp_timer_start_periodic(remaining_timer_, 1000000);
        }
    } else {
        if (esp_timer_is_active(remaining_timer_)) {
            esp_timer_stop(remaining_timer_);
        }
    }
}

}  // namespace bridge

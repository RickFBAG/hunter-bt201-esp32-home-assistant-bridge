#pragma once

#include <atomic>
#include <string>

#include "esp_err.h"
#include "esp_timer.h"

#include "freertos/queue.h"
#include "freertos/task.h"

#include "ble_transport.h"
#include "bridge_types.h"
#include "state_store.h"

namespace bridge {

class BridgeEventSink {
   public:
    virtual ~BridgeEventSink() = default;
    virtual void on_state_changed() = 0;
};

class CommandCoordinator {
   public:
    CommandCoordinator(BleTransport &transport, StateStore &state_store);
    ~CommandCoordinator();

    esp_err_t init();
    void set_event_sink(BridgeEventSink *sink);

    bool request_start(ZoneId zone);
    bool request_stop(ZoneId zone);
    bool request_apply_schedule(ZoneId zone, ApplyTarget target);
    bool request_refresh_battery();

    bool update_manual_duration(ZoneId zone, std::uint32_t seconds, std::string &error);
    bool update_timer_draft(ZoneId zone, const TimerScheduleDraft &draft, std::string &error);
    bool update_cycling_draft(ZoneId zone, const CyclingScheduleDraft &draft, std::string &error);

    void fail_safe_reset(std::string_view reason, bool error_state);

    const PersistedBridgeState &state() const;
    bool is_busy() const;

   private:
    enum class CommandType : std::uint8_t {
        Start = 0,
        Stop,
        ApplyTimer,
        ApplyCycling,
        RefreshBattery,
    };

    struct Command {
        CommandType type{CommandType::RefreshBattery};
        ZoneId zone{ZoneId::Zone1};
    };

    static void worker_task(void *param);
    static void remaining_timer_cb(void *arg);

    bool enqueue_command(Command command, bool high_priority);
    void task_loop();
    void notify_sink();
    std::int64_t now_ms() const;
    void on_transport_disconnect(int reason);
    bool perform_start(ZoneId zone);
    bool perform_start_attempt(ZoneId zone, std::uint32_t requested_seconds);
    bool perform_stop(ZoneId requested_zone);
    bool perform_stop_attempt();
    bool perform_apply_schedule(ZoneId zone, ApplyTarget target);
    bool perform_refresh_battery();
    bool open_session();
    void close_session();
    void update_battery_from_session();
    void mark_running(ZoneId zone, std::uint32_t remaining_seconds);
    void mark_idle_all(std::string_view reason);
    void mark_zone_status(ZoneId zone, ZoneRuntimeStatus status, std::string_view error = {});
    void refresh_remaining_counts();
    void ensure_remaining_timer_state();

    BleTransport &transport_;
    StateStore &state_store_;
    BridgeEventSink *sink_{nullptr};

    QueueHandle_t queue_{nullptr};
    TaskHandle_t task_handle_{nullptr};
    esp_timer_handle_t remaining_timer_{nullptr};

    std::atomic<bool> busy_{false};
    std::atomic<bool> urgent_stop_requested_{false};
    std::atomic<bool> transport_drop_detected_{false};
};

}  // namespace bridge

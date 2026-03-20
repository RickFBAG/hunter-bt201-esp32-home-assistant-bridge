#pragma once

#include <atomic>
#include <cstdint>
#include <string>

extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
}

struct cJSON;

#include "esp_err.h"
#include "mqtt_client.h"

#include "command_coordinator.h"

namespace bridge {

class MqttBridge : public BridgeEventSink {
   public:
    explicit MqttBridge(CommandCoordinator &coordinator);

    esp_err_t init();
    void on_state_changed() override;
    void handle_wifi_offline();
    void handle_wifi_online();
    bool is_connected() const;
    std::int64_t last_connection_change_ms() const;

   private:
    enum class WorkType : std::uint8_t {
        StateChanged = 0,
        WifiOffline,
        WifiOnline,
        MqttConnected,
        MqttDisconnected,
        MqttData,
    };

    struct WorkItem {
        WorkType type{WorkType::StateChanged};
        char topic[192]{};
        char payload[192]{};
    };

    static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
    static void worker_task(void *param);

    bool enqueue_work(const WorkItem &item);
    void worker_loop();
    void on_mqtt_connected();
    void on_mqtt_disconnected();
    void on_mqtt_data(const WorkItem &item);
    void subscribe_topics();
    void publish_discovery();
    void publish_state();
    void publish_availability(bool online);
    void publish_discovery_payload(const std::string &component, const std::string &object_id, cJSON *root);
    void publish_bridge_state();
    void publish_zone_state(ZoneId zone);
    void publish_timer_state(ZoneId zone);
    void publish_cycling_state(ZoneId zone);
    void publish_topic(const std::string &topic, const std::string &payload, bool retain = true);

    void process_command(const std::string &topic, const std::string &payload);
    void handle_zone_command(ZoneId zone, const std::string &suffix, const std::string &payload);
    void set_last_ui_error(std::string error);
    std::string build_topic(const std::string &suffix) const;
    std::string discovery_topic(const std::string &component, const std::string &object_id) const;

    CommandCoordinator &coordinator_;
    esp_mqtt_client_handle_t client_{nullptr};
    std::atomic<bool> connected_{false};
    std::atomic<bool> started_{false};
    std::atomic<std::int64_t> last_connection_change_ms_{0};
    QueueHandle_t work_queue_{nullptr};
    TaskHandle_t worker_task_handle_{nullptr};
    std::string last_ui_error_;
    std::string base_topic_;
    std::string discovery_prefix_;
    std::string device_id_;
    std::string availability_topic_;
};

}  // namespace bridge

#pragma once

#include <string>

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

   private:
    static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);

    void on_mqtt_connected();
    void on_mqtt_disconnected();
    void on_mqtt_data(esp_mqtt_event_handle_t event);
    void subscribe_topics();
    void publish_discovery();
    void publish_state();
    void publish_availability(bool online);
    void publish_bridge_state();
    void publish_zone_state(ZoneId zone);
    void publish_timer_state(ZoneId zone);
    void publish_cycling_state(ZoneId zone);
    void publish_topic(const std::string &topic, const std::string &payload, bool retain = true);

    void process_command(const std::string &topic, const std::string &payload);
    void handle_zone_command(ZoneId zone, const std::string &suffix, const std::string &payload);
    void set_last_ui_error(std::string error);
    std::string build_topic(const std::string &suffix) const;
    std::string discovery_topic() const;

    CommandCoordinator &coordinator_;
    esp_mqtt_client_handle_t client_{nullptr};
    bool connected_{false};
    std::string last_ui_error_;
    std::string base_topic_;
    std::string discovery_prefix_;
    std::string device_id_;
    std::string availability_topic_;
};

}  // namespace bridge

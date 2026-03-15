#include "mqtt_bridge.h"

#include <algorithm>
#include <cstdlib>
#include <utility>
#include <vector>

extern "C" {
#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
}

#include "bridge_config.h"
#include "hunter_protocol.h"

namespace bridge {

namespace {

constexpr char kTag[] = "MqttBridge";
constexpr char kPattern[] = "^(OFF|[0-2][0-9]:[0-5][0-9]:[0-5][0-9])$";

bool starts_with(const std::string &value, const std::string &prefix) {
    return value.rfind(prefix, 0) == 0;
}

std::string payload_from_event(esp_mqtt_event_handle_t event) {
    return std::string(event->data, event->data_len);
}

std::string topic_from_event(esp_mqtt_event_handle_t event) {
    return std::string(event->topic, event->topic_len);
}

std::string unique_id(const std::string &device_id, const std::string &component_id) {
    return device_id + "_" + component_id;
}

void add_device_block(cJSON *root) {
    auto *device = cJSON_AddObjectToObject(root, "device");
    cJSON_AddStringToObject(device, "name", config::kDeviceName);
    cJSON_AddStringToObject(device, "manufacturer", config::kDeviceManufacturer);
    cJSON_AddStringToObject(device, "model", config::kDeviceModel);
    cJSON_AddStringToObject(device, "sw_version", BRIDGE_PROJECT_VERSION);
    auto *identifiers = cJSON_AddArrayToObject(device, "identifiers");
    cJSON_AddItemToArray(identifiers, cJSON_CreateString(config::kDeviceId));
}

void add_origin_block(cJSON *root) {
    auto *origin = cJSON_AddObjectToObject(root, "origin");
    cJSON_AddStringToObject(origin, "name", "hunter-btt-bridge");
    cJSON_AddStringToObject(origin, "sw_version", BRIDGE_PROJECT_VERSION);
    cJSON_AddStringToObject(origin, "url", "https://www.home-assistant.io/integrations/mqtt");
}

void add_availability_block(cJSON *root, const std::string &topic) {
    auto *availability = cJSON_AddArrayToObject(root, "availability");
    auto *entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "topic", topic.c_str());
    cJSON_AddStringToObject(entry, "payload_available", "online");
    cJSON_AddStringToObject(entry, "payload_not_available", "offline");
    cJSON_AddItemToArray(availability, entry);
}

void add_component_common(cJSON *component, const std::string &name, const std::string &unique_id_value) {
    cJSON_AddStringToObject(component, "name", name.c_str());
    cJSON_AddStringToObject(component, "unique_id", unique_id_value.c_str());
}

void add_sensor_component(
    cJSON *components,
    const std::string &key,
    const std::string &name,
    const std::string &unique_id_value,
    const std::string &state_topic,
    const std::string &attributes_topic,
    const char *unit = nullptr) {
    auto *component = cJSON_AddObjectToObject(components, key.c_str());
    cJSON_AddStringToObject(component, "platform", "sensor");
    add_component_common(component, name, unique_id_value);
    cJSON_AddStringToObject(component, "state_topic", state_topic.c_str());
    if (!attributes_topic.empty()) {
        cJSON_AddStringToObject(component, "json_attributes_topic", attributes_topic.c_str());
    }
    if (unit != nullptr) {
        cJSON_AddStringToObject(component, "unit_of_measurement", unit);
    }
}

void add_button_component(
    cJSON *components,
    const std::string &key,
    const std::string &name,
    const std::string &unique_id_value,
    const std::string &command_topic) {
    auto *component = cJSON_AddObjectToObject(components, key.c_str());
    cJSON_AddStringToObject(component, "platform", "button");
    add_component_common(component, name, unique_id_value);
    cJSON_AddStringToObject(component, "command_topic", command_topic.c_str());
    cJSON_AddStringToObject(component, "payload_press", "PRESS");
}

void add_number_component(
    cJSON *components,
    const std::string &key,
    const std::string &name,
    const std::string &unique_id_value,
    const std::string &state_topic,
    const std::string &command_topic,
    int min_value,
    int max_value,
    int step,
    const char *unit = nullptr) {
    auto *component = cJSON_AddObjectToObject(components, key.c_str());
    cJSON_AddStringToObject(component, "platform", "number");
    add_component_common(component, name, unique_id_value);
    cJSON_AddStringToObject(component, "state_topic", state_topic.c_str());
    cJSON_AddStringToObject(component, "command_topic", command_topic.c_str());
    cJSON_AddStringToObject(component, "mode", "box");
    cJSON_AddNumberToObject(component, "min", min_value);
    cJSON_AddNumberToObject(component, "max", max_value);
    cJSON_AddNumberToObject(component, "step", step);
    if (unit != nullptr) {
        cJSON_AddStringToObject(component, "unit_of_measurement", unit);
    }
}

void add_text_component(
    cJSON *components,
    const std::string &key,
    const std::string &name,
    const std::string &unique_id_value,
    const std::string &state_topic,
    const std::string &command_topic,
    const char *pattern = nullptr) {
    auto *component = cJSON_AddObjectToObject(components, key.c_str());
    cJSON_AddStringToObject(component, "platform", "text");
    add_component_common(component, name, unique_id_value);
    cJSON_AddStringToObject(component, "state_topic", state_topic.c_str());
    cJSON_AddStringToObject(component, "command_topic", command_topic.c_str());
    cJSON_AddStringToObject(component, "mode", "text");
    if (pattern != nullptr) {
        cJSON_AddStringToObject(component, "pattern", pattern);
    }
}

void add_switch_component(
    cJSON *components,
    const std::string &key,
    const std::string &name,
    const std::string &unique_id_value,
    const std::string &state_topic,
    const std::string &command_topic) {
    auto *component = cJSON_AddObjectToObject(components, key.c_str());
    cJSON_AddStringToObject(component, "platform", "switch");
    add_component_common(component, name, unique_id_value);
    cJSON_AddStringToObject(component, "state_topic", state_topic.c_str());
    cJSON_AddStringToObject(component, "command_topic", command_topic.c_str());
    cJSON_AddStringToObject(component, "payload_on", "ON");
    cJSON_AddStringToObject(component, "payload_off", "OFF");
    cJSON_AddStringToObject(component, "state_on", "ON");
    cJSON_AddStringToObject(component, "state_off", "OFF");
}

}  // namespace

MqttBridge::MqttBridge(CommandCoordinator &coordinator)
    : coordinator_(coordinator),
      base_topic_(config::kBaseTopic),
      discovery_prefix_(config::kDiscoveryPrefix),
      device_id_(config::kDeviceId),
      availability_topic_(base_topic_ + "/availability") {}

esp_err_t MqttBridge::init() {
    esp_mqtt_client_config_t config_value = {};
    config_value.broker.address.uri = config::kMqttUri;
    config_value.credentials.username = config::kMqttUsername;
    config_value.credentials.authentication.password = config::kMqttPassword;
    config_value.session.last_will.topic = availability_topic_.c_str();
    config_value.session.last_will.msg = "offline";
    config_value.session.last_will.msg_len = 7;
    config_value.session.last_will.qos = 1;
    config_value.session.last_will.retain = 1;
    config_value.network.reconnect_timeout_ms = 5000;

    client_ = esp_mqtt_client_init(&config_value);
    if (client_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(client_, ESP_EVENT_ANY_ID, &MqttBridge::mqtt_event_handler, this));
    return esp_mqtt_client_start(client_);
}

void MqttBridge::on_state_changed() {
    if (connected_) {
        publish_state();
    }
}

void MqttBridge::handle_wifi_offline() {
    publish_availability(false);
}

void MqttBridge::handle_wifi_online() {
    if (connected_) {
        publish_availability(true);
        publish_discovery();
        publish_state();
    }
}

void MqttBridge::mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    auto *self = static_cast<MqttBridge *>(handler_args);
    auto *event = static_cast<esp_mqtt_event_handle_t>(event_data);
    switch (static_cast<esp_mqtt_event_id_t>(event_id)) {
        case MQTT_EVENT_CONNECTED:
            self->on_mqtt_connected();
            break;
        case MQTT_EVENT_DISCONNECTED:
            self->on_mqtt_disconnected();
            break;
        case MQTT_EVENT_DATA:
            self->on_mqtt_data(event);
            break;
        default:
            break;
    }
}

void MqttBridge::on_mqtt_connected() {
    connected_ = true;
    subscribe_topics();
    publish_availability(true);
    publish_discovery();
    publish_state();
}

void MqttBridge::on_mqtt_disconnected() {
    connected_ = false;
}

void MqttBridge::on_mqtt_data(esp_mqtt_event_handle_t event) {
    process_command(topic_from_event(event), payload_from_event(event));
}

void MqttBridge::subscribe_topics() {
    esp_mqtt_client_subscribe(client_, (base_topic_ + "/cmd/#").c_str(), 1);
    esp_mqtt_client_subscribe(client_, "homeassistant/status", 1);
}

void MqttBridge::publish_discovery() {
    auto *root = cJSON_CreateObject();
    add_device_block(root);
    add_origin_block(root);
    add_availability_block(root, availability_topic_);

    auto *components = cJSON_AddObjectToObject(root, "components");

    add_sensor_component(components, "bridge_health", "Bridge Health", unique_id(device_id_, "bridge_health"),
                         build_topic("state/bridge/health"), build_topic("attr/bridge"));
    add_sensor_component(components, "battery_percent", "Battery Percent", unique_id(device_id_, "battery_percent"),
                         build_topic("state/battery/percent"), build_topic("attr/bridge"), "%");
    add_button_component(components, "refresh_battery", "Refresh Battery", unique_id(device_id_, "refresh_battery"),
                         build_topic("cmd/battery/refresh"));

    for (std::size_t index = 0; index < kZoneCount; ++index) {
        const auto zone = zone_from_index(index);
        const auto zone_name = std::string(to_string(zone));

        add_button_component(components, zone_name + "_start", zone_name + " Start", unique_id(device_id_, zone_name + "_start"),
                             build_topic("cmd/" + zone_name + "/start"));
        add_button_component(components, zone_name + "_stop", zone_name + " Stop", unique_id(device_id_, zone_name + "_stop"),
                             build_topic("cmd/" + zone_name + "/stop"));
        add_number_component(components, zone_name + "_manual_duration", zone_name + " Manual Duration",
                             unique_id(device_id_, zone_name + "_manual_duration"),
                             build_topic("state/" + zone_name + "/manual_duration_seconds"),
                             build_topic("cmd/" + zone_name + "/manual_duration_seconds/set"),
                             1, kMaxWateringSeconds, 1, "s");
        add_sensor_component(components, zone_name + "_state", zone_name + " State", unique_id(device_id_, zone_name + "_state"),
                             build_topic("state/" + zone_name + "/runtime_state"), build_topic("attr/" + zone_name + "/runtime"));
        add_sensor_component(components, zone_name + "_remaining", zone_name + " Remaining Seconds",
                             unique_id(device_id_, zone_name + "_remaining"),
                             build_topic("state/" + zone_name + "/remaining_seconds"), build_topic("attr/" + zone_name + "/runtime"), "s");
        add_sensor_component(components, zone_name + "_active_schedule", zone_name + " Active Schedule Mode",
                             unique_id(device_id_, zone_name + "_active_schedule"),
                             build_topic("state/" + zone_name + "/active_schedule_mode"), build_topic("attr/" + zone_name + "/runtime"));

        add_switch_component(components, zone_name + "_timer_enabled", zone_name + " Timer Enabled",
                             unique_id(device_id_, zone_name + "_timer_enabled"),
                             build_topic("state/" + zone_name + "/timer/enabled"),
                             build_topic("cmd/" + zone_name + "/timer/enabled/set"));
        add_text_component(components, zone_name + "_timer_days", zone_name + " Timer Days",
                           unique_id(device_id_, zone_name + "_timer_days"),
                           build_topic("state/" + zone_name + "/timer/days"),
                           build_topic("cmd/" + zone_name + "/timer/days/set"));
        for (int slot = 0; slot < 4; ++slot) {
            const auto key = zone_name + "_timer_start" + std::to_string(slot + 1);
            add_text_component(components, key, zone_name + " Timer Start " + std::to_string(slot + 1), unique_id(device_id_, key),
                               build_topic("state/" + zone_name + "/timer/start" + std::to_string(slot + 1)),
                               build_topic("cmd/" + zone_name + "/timer/start" + std::to_string(slot + 1) + "/set"),
                               kPattern);
        }
        add_number_component(components, zone_name + "_timer_run", zone_name + " Timer Run Seconds",
                             unique_id(device_id_, zone_name + "_timer_run"),
                             build_topic("state/" + zone_name + "/timer/run_seconds"),
                             build_topic("cmd/" + zone_name + "/timer/run_seconds/set"),
                             1, kMaxWateringSeconds, 1, "s");
        add_button_component(components, zone_name + "_timer_apply", zone_name + " Timer Apply",
                             unique_id(device_id_, zone_name + "_timer_apply"),
                             build_topic("cmd/" + zone_name + "/timer/apply"));
        add_sensor_component(components, zone_name + "_timer_apply_status", zone_name + " Timer Apply Status",
                             unique_id(device_id_, zone_name + "_timer_apply_status"),
                             build_topic("state/" + zone_name + "/timer/apply_status"),
                             build_topic("attr/" + zone_name + "/timer"));

        add_switch_component(components, zone_name + "_cycling_enabled", zone_name + " Cycling Enabled",
                             unique_id(device_id_, zone_name + "_cycling_enabled"),
                             build_topic("state/" + zone_name + "/cycling/enabled"),
                             build_topic("cmd/" + zone_name + "/cycling/enabled/set"));
        add_text_component(components, zone_name + "_cycling_days", zone_name + " Cycling Days",
                           unique_id(device_id_, zone_name + "_cycling_days"),
                           build_topic("state/" + zone_name + "/cycling/days"),
                           build_topic("cmd/" + zone_name + "/cycling/days/set"));
        for (const auto &field : {"start1", "end1", "start2", "end2"}) {
            const auto key = zone_name + "_cycling_" + std::string(field);
            add_text_component(components, key, zone_name + " Cycling " + std::string(field), unique_id(device_id_, key),
                               build_topic("state/" + zone_name + "/cycling/" + field),
                               build_topic("cmd/" + zone_name + "/cycling/" + field + std::string("/set")),
                               kPattern);
        }
        add_number_component(components, zone_name + "_cycling_run", zone_name + " Cycling Run Seconds",
                             unique_id(device_id_, zone_name + "_cycling_run"),
                             build_topic("state/" + zone_name + "/cycling/run_seconds"),
                             build_topic("cmd/" + zone_name + "/cycling/run_seconds/set"),
                             1, kMaxWateringSeconds, 1, "s");
        add_number_component(components, zone_name + "_cycling_soak", zone_name + " Cycling Soak Seconds",
                             unique_id(device_id_, zone_name + "_cycling_soak"),
                             build_topic("state/" + zone_name + "/cycling/soak_seconds"),
                             build_topic("cmd/" + zone_name + "/cycling/soak_seconds/set"),
                             0, kMaxWateringSeconds, 1, "s");
        add_button_component(components, zone_name + "_cycling_apply", zone_name + " Cycling Apply",
                             unique_id(device_id_, zone_name + "_cycling_apply"),
                             build_topic("cmd/" + zone_name + "/cycling/apply"));
        add_sensor_component(components, zone_name + "_cycling_apply_status", zone_name + " Cycling Apply Status",
                             unique_id(device_id_, zone_name + "_cycling_apply_status"),
                             build_topic("state/" + zone_name + "/cycling/apply_status"),
                             build_topic("attr/" + zone_name + "/cycling"));
    }

    char *serialized = cJSON_PrintUnformatted(root);
    publish_topic(discovery_topic(), serialized, true);
    cJSON_free(serialized);
    cJSON_Delete(root);
}

void MqttBridge::publish_state() {
    publish_bridge_state();
    for (std::size_t index = 0; index < kZoneCount; ++index) {
        const auto zone = zone_from_index(index);
        publish_zone_state(zone);
        publish_timer_state(zone);
        publish_cycling_state(zone);
    }
}

void MqttBridge::publish_availability(const bool online) {
    publish_topic(availability_topic_, online ? "online" : "offline", true);
}

void MqttBridge::publish_bridge_state() {
    const auto &state = coordinator_.state();
    const auto health = coordinator_.is_busy() ? BridgeHealth::Busy :
                        (!std::string(state.bridge_error.data()).empty() || !last_ui_error_.empty() ? BridgeHealth::Error : BridgeHealth::Ok);
    publish_topic(build_topic("state/bridge/health"), to_string(health), true);
    publish_topic(build_topic("state/battery/percent"), std::to_string(state.battery_percent), true);

    auto *attrs = cJSON_CreateObject();
    cJSON_AddStringToObject(attrs, "bridge_error", state.bridge_error.data());
    cJSON_AddStringToObject(attrs, "last_ui_error", last_ui_error_.c_str());
    cJSON_AddBoolToObject(attrs, "busy", coordinator_.is_busy());
    cJSON_AddNumberToObject(attrs, "battery_updated_epoch_ms", state.battery_updated_epoch_ms);
    char *serialized = cJSON_PrintUnformatted(attrs);
    publish_topic(build_topic("attr/bridge"), serialized, true);
    cJSON_free(serialized);
    cJSON_Delete(attrs);
}

void MqttBridge::publish_zone_state(const ZoneId zone) {
    const auto &zone_state = coordinator_.state().zones[to_index(zone)];
    const auto zone_name = std::string(to_string(zone));
    publish_topic(build_topic("state/" + zone_name + "/manual_duration_seconds"), std::to_string(zone_state.manual_duration_seconds), true);
    publish_topic(build_topic("state/" + zone_name + "/runtime_state"), to_string(zone_state.runtime_status), true);
    publish_topic(build_topic("state/" + zone_name + "/remaining_seconds"), std::to_string(zone_state.remaining_seconds), true);
    publish_topic(build_topic("state/" + zone_name + "/active_schedule_mode"), to_string(zone_state.active_schedule_mode), true);

    auto *attrs = cJSON_CreateObject();
    cJSON_AddBoolToObject(attrs, "confirmed_state_stale", zone_state.confirmed_state_stale);
    cJSON_AddStringToObject(attrs, "last_error", zone_state.last_error.data());
    cJSON_AddNumberToObject(attrs, "last_confirmed_epoch_ms", zone_state.last_confirmed_epoch_ms);
    cJSON_AddNumberToObject(attrs, "expected_end_epoch_ms", zone_state.expected_end_epoch_ms);
    char *serialized = cJSON_PrintUnformatted(attrs);
    publish_topic(build_topic("attr/" + zone_name + "/runtime"), serialized, true);
    cJSON_free(serialized);
    cJSON_Delete(attrs);
}

void MqttBridge::publish_timer_state(const ZoneId zone) {
    const auto &zone_state = coordinator_.state().zones[to_index(zone)];
    const auto zone_name = std::string(to_string(zone));
    publish_topic(build_topic("state/" + zone_name + "/timer/enabled"), zone_state.timer.enabled ? "ON" : "OFF", true);
    publish_topic(build_topic("state/" + zone_name + "/timer/days"), protocol::format_days_csv(zone_state.timer.days_mask), true);
    for (std::size_t slot = 0; slot < zone_state.timer.start_times.size(); ++slot) {
        publish_topic(build_topic("state/" + zone_name + "/timer/start" + std::to_string(slot + 1)),
                      protocol::format_time_string(zone_state.timer.start_times[slot]), true);
    }
    publish_topic(build_topic("state/" + zone_name + "/timer/run_seconds"), std::to_string(zone_state.timer.run_seconds), true);
    publish_topic(build_topic("state/" + zone_name + "/timer/apply_status"), zone_state.last_apply_status.data(), true);

    auto *attrs = cJSON_CreateObject();
    cJSON_AddStringToObject(attrs, "origin", to_string(zone_state.timer.origin));
    cJSON_AddBoolToObject(attrs, "confirmed_state_stale", zone_state.confirmed_state_stale);
    cJSON_AddStringToObject(attrs, "last_error", zone_state.last_error.data());
    cJSON_AddStringToObject(attrs, "last_config_hex",
                            protocol::bytes_to_hex(zone_state.last_config_bytes.data(), zone_state.last_config_len).c_str());
    cJSON_AddStringToObject(attrs, "last_block_hex",
                            protocol::bytes_to_hex(zone_state.last_timer_block_bytes.data(), zone_state.last_timer_block_len).c_str());
    char *serialized = cJSON_PrintUnformatted(attrs);
    publish_topic(build_topic("attr/" + zone_name + "/timer"), serialized, true);
    cJSON_free(serialized);
    cJSON_Delete(attrs);
}

void MqttBridge::publish_cycling_state(const ZoneId zone) {
    const auto &zone_state = coordinator_.state().zones[to_index(zone)];
    const auto zone_name = std::string(to_string(zone));
    publish_topic(build_topic("state/" + zone_name + "/cycling/enabled"), zone_state.cycling.enabled ? "ON" : "OFF", true);
    publish_topic(build_topic("state/" + zone_name + "/cycling/days"), protocol::format_days_csv(zone_state.cycling.days_mask), true);
    publish_topic(build_topic("state/" + zone_name + "/cycling/start1"), protocol::format_time_string(zone_state.cycling.start1), true);
    publish_topic(build_topic("state/" + zone_name + "/cycling/end1"), protocol::format_time_string(zone_state.cycling.end1), true);
    publish_topic(build_topic("state/" + zone_name + "/cycling/start2"), protocol::format_time_string(zone_state.cycling.start2), true);
    publish_topic(build_topic("state/" + zone_name + "/cycling/end2"), protocol::format_time_string(zone_state.cycling.end2), true);
    publish_topic(build_topic("state/" + zone_name + "/cycling/run_seconds"), std::to_string(zone_state.cycling.run_seconds), true);
    publish_topic(build_topic("state/" + zone_name + "/cycling/soak_seconds"), std::to_string(zone_state.cycling.soak_seconds), true);
    publish_topic(build_topic("state/" + zone_name + "/cycling/apply_status"), zone_state.last_apply_status.data(), true);

    auto *attrs = cJSON_CreateObject();
    cJSON_AddStringToObject(attrs, "origin", to_string(zone_state.cycling.origin));
    cJSON_AddBoolToObject(attrs, "confirmed_state_stale", zone_state.confirmed_state_stale);
    cJSON_AddStringToObject(attrs, "last_error", zone_state.last_error.data());
    cJSON_AddStringToObject(attrs, "last_config_hex",
                            protocol::bytes_to_hex(zone_state.last_config_bytes.data(), zone_state.last_config_len).c_str());
    cJSON_AddStringToObject(attrs, "last_block_hex",
                            protocol::bytes_to_hex(zone_state.last_cycling_block_bytes.data(), zone_state.last_cycling_block_len).c_str());
    char *serialized = cJSON_PrintUnformatted(attrs);
    publish_topic(build_topic("attr/" + zone_name + "/cycling"), serialized, true);
    cJSON_free(serialized);
    cJSON_Delete(attrs);
}

void MqttBridge::publish_topic(const std::string &topic, const std::string &payload, const bool retain) {
    if (!connected_ || client_ == nullptr) {
        return;
    }
    esp_mqtt_client_publish(client_, topic.c_str(), payload.c_str(), 0, 1, retain ? 1 : 0);
}

void MqttBridge::process_command(const std::string &topic, const std::string &payload) {
    if (topic == "homeassistant/status") {
        if (payload == "online") {
            publish_discovery();
            publish_state();
        }
        return;
    }

    const auto prefix = base_topic_ + "/cmd/";
    if (!starts_with(topic, prefix)) {
        return;
    }

    const auto suffix = topic.substr(prefix.size());
    if (suffix == "battery/refresh" && payload == "PRESS") {
        if (!coordinator_.request_refresh_battery()) {
            set_last_ui_error("battery refresh rejected: busy");
        } else {
            last_ui_error_.clear();
        }
        return;
    }

    if (starts_with(suffix, "zone1/")) {
        handle_zone_command(ZoneId::Zone1, suffix.substr(6), payload);
    } else if (starts_with(suffix, "zone2/")) {
        handle_zone_command(ZoneId::Zone2, suffix.substr(6), payload);
    }
}

void MqttBridge::handle_zone_command(const ZoneId zone, const std::string &suffix, const std::string &payload) {
    std::string error;
    const auto &state = coordinator_.state();
    auto timer = state.zones[to_index(zone)].timer;
    auto cycling = state.zones[to_index(zone)].cycling;

    if (suffix == "start" && payload == "PRESS") {
        if (!coordinator_.request_start(zone)) {
            set_last_ui_error("start rejected: busy");
        } else {
            last_ui_error_.clear();
        }
        return;
    }
    if (suffix == "stop" && payload == "PRESS") {
        if (!coordinator_.request_stop(zone)) {
            set_last_ui_error("stop rejected");
        } else {
            last_ui_error_.clear();
        }
        return;
    }
    if (suffix == "manual_duration_seconds/set") {
        const auto value = static_cast<std::uint32_t>(std::strtoul(payload.c_str(), nullptr, 10));
        if (!coordinator_.update_manual_duration(zone, value, error)) {
            set_last_ui_error(error);
        } else {
            last_ui_error_.clear();
        }
        return;
    }

    if (suffix == "timer/enabled/set") {
        timer.enabled = payload == "ON";
        if (!coordinator_.update_timer_draft(zone, timer, error)) {
            set_last_ui_error(error);
        } else {
            last_ui_error_.clear();
        }
        return;
    }
    if (suffix == "timer/days/set") {
        std::uint8_t mask = 0;
        const auto parsed = protocol::parse_days_csv(payload, mask);
        if (!parsed.ok) {
            set_last_ui_error(parsed.error);
            return;
        }
        timer.days_mask = mask;
        if (!coordinator_.update_timer_draft(zone, timer, error)) {
            set_last_ui_error(error);
        } else {
            last_ui_error_.clear();
        }
        return;
    }
    if (suffix == "timer/start1/set" || suffix == "timer/start2/set" || suffix == "timer/start3/set" || suffix == "timer/start4/set") {
        const auto slot = static_cast<std::size_t>(suffix[11] - '1');
        std::int32_t seconds = 0;
        const auto parsed = protocol::parse_time_string(payload, seconds);
        if (!parsed.ok || slot >= timer.start_times.size()) {
            set_last_ui_error(parsed.ok ? "invalid timer slot" : parsed.error);
            return;
        }
        timer.start_times[slot] = seconds;
        if (!coordinator_.update_timer_draft(zone, timer, error)) {
            set_last_ui_error(error);
        } else {
            last_ui_error_.clear();
        }
        return;
    }
    if (suffix == "timer/run_seconds/set") {
        timer.run_seconds = static_cast<std::uint32_t>(std::strtoul(payload.c_str(), nullptr, 10));
        if (!coordinator_.update_timer_draft(zone, timer, error)) {
            set_last_ui_error(error);
        } else {
            last_ui_error_.clear();
        }
        return;
    }
    if (suffix == "timer/apply" && payload == "PRESS") {
        if (!coordinator_.request_apply_schedule(zone, ApplyTarget::Timer)) {
            set_last_ui_error("timer apply rejected: busy");
        } else {
            last_ui_error_.clear();
        }
        return;
    }

    if (suffix == "cycling/enabled/set") {
        cycling.enabled = payload == "ON";
        if (!coordinator_.update_cycling_draft(zone, cycling, error)) {
            set_last_ui_error(error);
        } else {
            last_ui_error_.clear();
        }
        return;
    }
    if (suffix == "cycling/days/set") {
        std::uint8_t mask = 0;
        const auto parsed = protocol::parse_days_csv(payload, mask);
        if (!parsed.ok) {
            set_last_ui_error(parsed.error);
            return;
        }
        cycling.days_mask = mask;
        if (!coordinator_.update_cycling_draft(zone, cycling, error)) {
            set_last_ui_error(error);
        } else {
            last_ui_error_.clear();
        }
        return;
    }
    if (suffix == "cycling/start1/set" || suffix == "cycling/end1/set" || suffix == "cycling/start2/set" || suffix == "cycling/end2/set") {
        std::int32_t seconds = 0;
        const auto parsed = protocol::parse_time_string(payload, seconds);
        if (!parsed.ok) {
            set_last_ui_error(parsed.error);
            return;
        }
        if (suffix == "cycling/start1/set") cycling.start1 = seconds;
        if (suffix == "cycling/end1/set") cycling.end1 = seconds;
        if (suffix == "cycling/start2/set") cycling.start2 = seconds;
        if (suffix == "cycling/end2/set") cycling.end2 = seconds;
        if (!coordinator_.update_cycling_draft(zone, cycling, error)) {
            set_last_ui_error(error);
        } else {
            last_ui_error_.clear();
        }
        return;
    }
    if (suffix == "cycling/run_seconds/set") {
        cycling.run_seconds = static_cast<std::uint32_t>(std::strtoul(payload.c_str(), nullptr, 10));
        if (!coordinator_.update_cycling_draft(zone, cycling, error)) {
            set_last_ui_error(error);
        } else {
            last_ui_error_.clear();
        }
        return;
    }
    if (suffix == "cycling/soak_seconds/set") {
        cycling.soak_seconds = static_cast<std::uint32_t>(std::strtoul(payload.c_str(), nullptr, 10));
        if (!coordinator_.update_cycling_draft(zone, cycling, error)) {
            set_last_ui_error(error);
        } else {
            last_ui_error_.clear();
        }
        return;
    }
    if (suffix == "cycling/apply" && payload == "PRESS") {
        if (!coordinator_.request_apply_schedule(zone, ApplyTarget::Cycling)) {
            set_last_ui_error("cycling apply rejected: busy");
        } else {
            last_ui_error_.clear();
        }
    }
}

void MqttBridge::set_last_ui_error(std::string error) {
    ESP_LOGW(kTag, "%s", error.c_str());
    last_ui_error_ = std::move(error);
    publish_bridge_state();
}

std::string MqttBridge::build_topic(const std::string &suffix) const {
    return base_topic_ + "/" + suffix;
}

std::string MqttBridge::discovery_topic() const {
    return discovery_prefix_ + "/device/" + device_id_ + "/config";
}

}  // namespace bridge

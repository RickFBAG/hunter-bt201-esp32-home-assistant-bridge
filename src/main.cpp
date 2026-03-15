#include <cstring>

extern "C" {
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
}

#include "ble_transport.h"
#include "bridge_config.h"
#include "command_coordinator.h"
#include "mqtt_bridge.h"
#include "state_store.h"

namespace {

constexpr char kTag[] = "Main";
constexpr int kWifiConnectedBit = BIT0;

EventGroupHandle_t g_wifi_events = nullptr;
bridge::CommandCoordinator *g_coordinator = nullptr;
bridge::MqttBridge *g_mqtt_bridge = nullptr;

void battery_refresh_cb(void *arg) {
    auto *coordinator = static_cast<bridge::CommandCoordinator *>(arg);
    coordinator->request_refresh_battery();
}

void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(g_wifi_events, kWifiConnectedBit);
        if (g_coordinator != nullptr) {
            g_coordinator->fail_safe_reset("wifi_disconnected", false);
        }
        if (g_mqtt_bridge != nullptr) {
            g_mqtt_bridge->handle_wifi_offline();
        }
        esp_wifi_connect();
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(g_wifi_events, kWifiConnectedBit);
        if (g_mqtt_bridge != nullptr) {
            g_mqtt_bridge->handle_wifi_online();
        }
    }
}

void init_wifi() {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));

    esp_event_handler_instance_t wifi_handler = nullptr;
    esp_event_handler_instance_t ip_handler = nullptr;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, &wifi_handler));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr, &ip_handler));

    wifi_config_t wifi_config = {};
    std::strncpy(reinterpret_cast<char *>(wifi_config.sta.ssid), bridge::config::kWifiSsid, sizeof(wifi_config.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char *>(wifi_config.sta.password), bridge::config::kWifiPassword, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

}  // namespace

extern "C" void app_main(void) {
    ESP_LOGI(kTag, "Starting hunter BT201 bridge");
    g_wifi_events = xEventGroupCreate();

    static bridge::StateStore state_store;
    ESP_ERROR_CHECK(state_store.init());
    state_store.mark_boot_stale();
    ESP_ERROR_CHECK(state_store.save());

    static bridge::BleTransport transport;
    ESP_ERROR_CHECK(transport.init());

    static bridge::CommandCoordinator coordinator(transport, state_store);
    ESP_ERROR_CHECK(coordinator.init());
    g_coordinator = &coordinator;

    static bridge::MqttBridge mqtt_bridge(coordinator);
    coordinator.set_event_sink(&mqtt_bridge);
    g_mqtt_bridge = &mqtt_bridge;

    init_wifi();
    ESP_ERROR_CHECK(mqtt_bridge.init());

    esp_timer_create_args_t battery_timer_args = {};
    battery_timer_args.callback = &battery_refresh_cb;
    battery_timer_args.arg = &coordinator;
    battery_timer_args.dispatch_method = ESP_TIMER_TASK;
    battery_timer_args.name = "battery_refresh";
    battery_timer_args.skip_unhandled_events = true;

    esp_timer_handle_t battery_timer = nullptr;
    ESP_ERROR_CHECK(esp_timer_create(&battery_timer_args, &battery_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(battery_timer, bridge::config::kBatteryRefreshIntervalMs * 1000ULL));
    coordinator.request_refresh_battery();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

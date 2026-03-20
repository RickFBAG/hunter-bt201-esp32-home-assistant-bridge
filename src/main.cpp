#include <array>
#include <cstdio>
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
#include "status_display.h"
#include "state_store.h"

namespace {

constexpr char kTag[] = "Main";
constexpr int kWifiConnectedBit = BIT0;
constexpr std::uint32_t kDisplayTaskStackSize = 16384;

EventGroupHandle_t g_wifi_events = nullptr;
bridge::CommandCoordinator *g_coordinator = nullptr;
bridge::MqttBridge *g_mqtt_bridge = nullptr;
std::array<char, 16> g_wifi_ip = {"-"};

void battery_refresh_cb(void *arg) {
    auto *coordinator = static_cast<bridge::CommandCoordinator *>(arg);
    coordinator->request_refresh_battery();
}

void status_display_task(void *arg) {
    auto *display = static_cast<bridge::StatusDisplay *>(arg);
    static bridge::DisplaySnapshot display_snapshot{};
    ESP_LOGI(kTag, "Status display task started");
    while (true) {
        if (g_coordinator != nullptr) {
            display_snapshot.state = g_coordinator->state();
            display_snapshot.ble = g_coordinator->ble_telemetry();
            display_snapshot.bridge_busy = g_coordinator->is_busy();
        }
        display_snapshot.wifi_connected = (xEventGroupGetBits(g_wifi_events) & kWifiConnectedBit) != 0;
        display_snapshot.wifi_ip = g_wifi_ip;
        if (g_mqtt_bridge != nullptr) {
            display_snapshot.mqtt_connected = g_mqtt_bridge->is_connected();
            display_snapshot.mqtt_last_change_ms = g_mqtt_bridge->last_connection_change_ms();
        }
        if (!display->is_initialized()) {
            ESP_LOGW(kTag, "Status display no longer initialized; render loop paused");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        display->render(display_snapshot, esp_timer_get_time() / 1000);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(g_wifi_events, kWifiConnectedBit);
        std::snprintf(g_wifi_ip.data(), g_wifi_ip.size(), "-");
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
        auto *got_ip = static_cast<ip_event_got_ip_t *>(event_data);
        xEventGroupSetBits(g_wifi_events, kWifiConnectedBit);
        std::snprintf(g_wifi_ip.data(), g_wifi_ip.size(), IPSTR, IP2STR(&got_ip->ip_info.ip));
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
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(kTag, "WiFi power save disabled");
}

}  // namespace

extern "C" void app_main(void) {
    ESP_LOGI(kTag, "Starting hunter BTT201 bridge");
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

    static bridge::StatusDisplay status_display;

    init_wifi();
    const auto mqtt_init_result = mqtt_bridge.init();
    if (mqtt_init_result != ESP_OK) {
        ESP_LOGE(kTag, "MQTT bridge init failed: %s (0x%x)", esp_err_to_name(mqtt_init_result), static_cast<unsigned>(mqtt_init_result));
        ESP_LOGE(kTag, "Fix BRIDGE_MQTT_URI in bridge_secrets.h and reboot; bridge will stay offline until then");
        coordinator.fail_safe_reset("mqtt_init_failed", true);
    }

    const auto display_init_result = status_display.init();
    if (display_init_result != ESP_OK) {
        ESP_LOGE(kTag, "Status display init failed: %s (0x%x)", esp_err_to_name(display_init_result), static_cast<unsigned>(display_init_result));
    } else {
        const auto created = xTaskCreate(
            &status_display_task,
            "status_display",
            kDisplayTaskStackSize,
            &status_display,
            1,
            nullptr);
        if (created != pdPASS) {
            ESP_LOGE(kTag, "Failed to start status display task");
        }
    }

    esp_timer_create_args_t battery_timer_args = {};
    battery_timer_args.callback = &battery_refresh_cb;
    battery_timer_args.arg = &coordinator;
    battery_timer_args.dispatch_method = ESP_TIMER_TASK;
    battery_timer_args.name = "battery_refresh";
    battery_timer_args.skip_unhandled_events = true;

    esp_timer_handle_t battery_timer = nullptr;
    ESP_ERROR_CHECK(esp_timer_create(&battery_timer_args, &battery_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(battery_timer, bridge::config::kBatteryRefreshIntervalMs * 1000ULL));

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

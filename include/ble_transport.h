#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "esp_err.h"

#include "bridge_types.h"

extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
}

namespace bridge {

class BleTransport {
   public:
    BleTransport();
    ~BleTransport();

    esp_err_t init();
    bool connect();
    void disconnect();
    bool is_connected() const;

    bool read_characteristic(const char *uuid, std::vector<std::uint8_t> &out);
    bool write_characteristic(const char *uuid, const std::uint8_t *data, std::size_t len);
    bool write_characteristic(const char *uuid, const std::vector<std::uint8_t> &data);
    bool read_battery_percent(std::uint8_t &percent_out);

    void clear_notifications();
    bool wait_for_notification(NotificationEvent &event, std::uint32_t timeout_ms);

    void set_disconnect_callback(std::function<void(int)> callback);
    const std::string &last_error() const;

   private:
    struct RawNotification {
        std::uint16_t attr_handle{0};
        std::uint8_t payload[20]{};
        std::size_t payload_len{0};
    };

    struct GattHandles {
        std::uint16_t ff81{0};
        std::uint16_t ff82{0};
        std::uint16_t ff83{0};
        std::uint16_t ff84{0};
        std::uint16_t ff86{0};
        std::uint16_t ff87{0};
        std::uint16_t ff88{0};
        std::uint16_t ff89{0};
        std::uint16_t ff8a{0};
        std::uint16_t ff8b{0};
        std::uint16_t ff8c{0};
        std::uint16_t ff8d{0};
        std::uint16_t ff8e{0};
        std::uint16_t ff8f{0};
        std::uint16_t battery_level{0};
    };

    bool ensure_ble_ready();
    bool scan_for_target();
    bool connect_once();
    bool discover_handles();
    bool discover_service_range(std::uint16_t service_uuid, std::uint16_t &start, std::uint16_t &end);
    bool discover_characteristics(std::uint16_t start, std::uint16_t end);
    bool enable_notification(std::uint16_t value_handle);
    bool read_handle(std::uint16_t handle, std::vector<std::uint8_t> &out);
    bool write_handle(std::uint16_t handle, const std::uint8_t *data, std::size_t len);
    std::uint16_t handle_for_uuid(const char *uuid) const;
    std::string uuid_for_handle(std::uint16_t handle) const;
    void set_error(std::string message);
    void reset_handles();

    static void host_task(void *param);
    static void on_reset(int reason);
    static void on_sync();
    static int gap_event(struct ble_gap_event *event, void *arg);
    static int service_discovery_cb(
        std::uint16_t conn_handle,
        const struct ble_gatt_error *error,
        const struct ble_gatt_svc *service,
        void *arg);
    static int characteristic_discovery_cb(
        std::uint16_t conn_handle,
        const struct ble_gatt_error *error,
        const struct ble_gatt_chr *chr,
        void *arg);
    static int read_cb(
        std::uint16_t conn_handle,
        const struct ble_gatt_error *error,
        struct ble_gatt_attr *attr,
        void *arg);
    static int write_cb(
        std::uint16_t conn_handle,
        const struct ble_gatt_error *error,
        struct ble_gatt_attr *attr,
        void *arg);

    static BleTransport *instance_;

    bool initialized_{false};
    bool synced_{false};
    std::uint8_t own_addr_type_{0};
    std::uint16_t conn_handle_{0xFFFF};
    std::string target_mac_;
    std::string target_name_hint_;
    std::string last_error_;
    GattHandles handles_{};

    ble_addr_t peer_addr_{};
    bool target_found_{false};
    ble_addr_t fallback_addr_{};
    bool fallback_found_{false};
    int fallback_score_{0};

    SemaphoreHandle_t scan_done_sem_{nullptr};
    SemaphoreHandle_t connect_sem_{nullptr};
    SemaphoreHandle_t gatt_sem_{nullptr};
    QueueHandle_t notify_queue_{nullptr};
    SemaphoreHandle_t mutex_{nullptr};

    int last_status_{0};
    std::vector<std::uint8_t> last_read_payload_{};
    std::uint16_t pending_service_start_{0};
    std::uint16_t pending_service_end_{0};
    bool service_found_{false};
    std::function<void(int)> disconnect_callback_{};
};

}  // namespace bridge

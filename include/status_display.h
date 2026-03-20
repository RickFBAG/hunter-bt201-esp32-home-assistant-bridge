#pragma once

#include <array>
#include <cstdint>

#include "esp_err.h"

#include "bridge_types.h"
#include "command_coordinator.h"

namespace bridge {

struct DisplaySnapshot {
    PersistedBridgeState state{};
    BleTelemetrySnapshot ble{};
    bool bridge_busy{false};
    bool wifi_connected{false};
    std::array<char, 16> wifi_ip{};
    bool mqtt_connected{false};
    std::int64_t mqtt_last_change_ms{0};
};

class StatusDisplay {
   public:
    StatusDisplay() = default;
    ~StatusDisplay();

    esp_err_t init();
    bool is_initialized() const;
    void render(const DisplaySnapshot &snapshot, std::int64_t now_ms);

   private:
    bool init_i2c_bus();
    bool enable_panel_power();
    bool init_board_power_monitor();
    bool init_wake_inputs();
    bool read_i2c_register(std::uint8_t address, std::uint8_t reg, std::uint8_t &value_out);
    bool write_i2c_register(std::uint8_t address, std::uint8_t reg, std::uint8_t value);
    bool refresh_board_power(std::int64_t now_ms);
    void maybe_refresh_board_power(std::int64_t now_ms);
    bool poll_power_key_wake_request();
    void update_display_power(std::int64_t now_ms);
    void wake_display(std::int64_t now_ms);
    void sleep_display();
    bool init_spi_bus();
    bool init_panel();
    bool set_panel_enabled(bool enabled);
    bool wait_for_panel_idle();
    void render_solid_color(std::uint16_t color);
    void render_frame(const DisplaySnapshot &snapshot, std::int64_t now_ms);

    bool initialized_{false};
    bool first_render_done_{false};
    bool i2c_ready_{false};
    bool spi_ready_{false};
    bool pmu_ready_{false};
    bool wake_inputs_ready_{false};
    bool display_awake_{true};
    bool force_render_{true};
    bool boot_button_pressed_{false};
    bool boot_restart_armed_{false};
    bool board_battery_present_{false};
    bool board_battery_charging_{false};
    bool board_usb_present_{false};
    std::uint8_t board_battery_percent_{0};
    std::uint16_t board_battery_mv_{0};
    std::uint16_t board_system_mv_{0};
    std::int64_t display_awake_until_ms_{10000};
    std::int64_t last_render_ms_{0};
    std::int64_t board_power_last_update_ms_{0};
    void *panel_io_{nullptr};
    void *panel_{nullptr};
    std::uint16_t *transfer_buffer_{nullptr};
};

}  // namespace bridge

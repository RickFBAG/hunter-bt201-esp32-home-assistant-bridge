#include "status_display.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_sh8601.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace bridge {

namespace {

constexpr char kTag[] = "StatusDisplay";

constexpr int kDisplayWidth = 368;
constexpr int kDisplayHeight = 448;
constexpr int kTransferLines = 16;
constexpr int kHeaderScale = 4;
constexpr int kSectionScale = 2;
constexpr int kBodyScale = 2;
constexpr int kHeaderY = 18;
constexpr int kOuterPad = 14;
constexpr int kSectionTitleY = 76;
constexpr int kSectionLineY1 = 102;
constexpr int kSectionLineY2 = 124;
constexpr int kSection2TitleY = 164;
constexpr int kSection2LineY1 = 190;
constexpr int kSection2LineY2 = 212;
constexpr int kSection2LineY3 = 234;
constexpr int kSection2LineY4 = 256;
constexpr int kSection3TitleY = 312;
constexpr int kSection3LineY1 = 338;
constexpr int kSection3LineY2 = 360;
constexpr int kSection3LineY3 = 382;
constexpr int kSection3LineY4 = 404;
constexpr int kBodyLineHeight = 22;
constexpr int kMaxBodyChars = 26;
constexpr int kTransferBufferPixels = kDisplayWidth * kTransferLines;
constexpr std::int64_t kBoardPowerRefreshMs = 30000;
constexpr std::int64_t kDisplayWakeDurationMs = 10000;

constexpr spi_host_device_t kSpiHost = SPI2_HOST;
constexpr i2c_port_t kI2cHost = I2C_NUM_0;
constexpr gpio_num_t kPinBootButton = GPIO_NUM_9;
constexpr gpio_num_t kPinLcdCs = GPIO_NUM_5;
constexpr gpio_num_t kPinLcdPclk = GPIO_NUM_0;
constexpr gpio_num_t kPinLcdData0 = GPIO_NUM_1;
constexpr gpio_num_t kPinLcdData1 = GPIO_NUM_2;
constexpr gpio_num_t kPinLcdData2 = GPIO_NUM_3;
constexpr gpio_num_t kPinLcdData3 = GPIO_NUM_4;
constexpr gpio_num_t kPinTouchScl = GPIO_NUM_7;
constexpr gpio_num_t kPinTouchSda = GPIO_NUM_8;

constexpr std::uint8_t kIoExpanderAddress = 0x20;
constexpr std::uint8_t kIoExpanderOutputReg = 0x01;
constexpr std::uint8_t kIoExpanderConfigReg = 0x03;
constexpr std::uint8_t kIoExpanderDisplayMask = 0x30;  // P4 + P5
constexpr std::uint8_t kIoExpanderConfigValue = 0xCF;  // P4/P5 outputs, others inputs
constexpr std::uint8_t kPmuAddress = 0x34;
constexpr std::uint8_t kPmuRegStatus1 = 0x00;
constexpr std::uint8_t kPmuRegStatus2 = 0x01;
constexpr std::uint8_t kPmuRegChipId = 0x03;
constexpr std::uint8_t kPmuRegAdcChannelCtrl = 0x30;
constexpr std::uint8_t kPmuRegIntEn2 = 0x41;
constexpr std::uint8_t kPmuRegIntStatus2 = 0x49;
constexpr std::uint8_t kPmuRegBattVoltageHigh = 0x34;
constexpr std::uint8_t kPmuRegBattVoltageLow = 0x35;
constexpr std::uint8_t kPmuRegSystemVoltageHigh = 0x3A;
constexpr std::uint8_t kPmuRegSystemVoltageLow = 0x3B;
constexpr std::uint8_t kPmuRegBattPercent = 0xA4;
constexpr std::uint8_t kPmuChipId = 0x4A;
constexpr std::uint8_t kPmuPkeyShortIrqMask = 0x08;

struct Glyph {
    std::array<std::uint8_t, 7> rows{};
};

constexpr std::uint16_t panel_color(const std::uint8_t red, const std::uint8_t green, const std::uint8_t blue) {
    return static_cast<std::uint16_t>(((red & 0xF8u) << 8u) | ((green & 0xFCu) << 3u) | (blue >> 3u));
}

constexpr std::uint16_t kColorWhite = 0xFFFF;
constexpr std::uint16_t kColorBlack = 0x0000;
constexpr std::uint16_t kColorBackground = kColorBlack;
constexpr std::uint16_t kColorAccent = panel_color(78, 186, 255);
constexpr std::uint16_t kColorText = panel_color(236, 241, 245);
constexpr std::uint16_t kColorMuted = panel_color(118, 126, 138);
constexpr std::uint16_t kColorOk = panel_color(101, 220, 145);
constexpr std::uint16_t kColorWarn = panel_color(242, 186, 72);
constexpr std::uint16_t kColorError = panel_color(255, 109, 91);

RTC_NOINIT_ATTR int s_last_render_stage = 0;

static const sh8601_lcd_init_cmd_t kLcdInitCmds[] = {
    {0x11, (std::uint8_t[]){0x00}, 0, 120},
    {0x44, (std::uint8_t[]){0x01, 0xD1}, 2, 0},
    {0x35, (std::uint8_t[]){0x00}, 1, 0},
    {0x53, (std::uint8_t[]){0x20}, 1, 10},
    {0x2A, (std::uint8_t[]){0x00, 0x00, 0x01, 0x6F}, 4, 0},
    {0x2B, (std::uint8_t[]){0x00, 0x00, 0x01, 0xBF}, 4, 0},
    {0x51, (std::uint8_t[]){0x00}, 1, 10},
    {0x29, (std::uint8_t[]){0x00}, 0, 10},
    {0x51, (std::uint8_t[]){0xFF}, 1, 0},
};

bool check_result(const char *const step, const esp_err_t result) {
    if (result == ESP_OK) {
        return true;
    }
    ESP_LOGE(kTag, "%s failed: %s", step, esp_err_to_name(result));
    return false;
}

char ascii_upper(const char ch) {
    return (ch >= 'a' && ch <= 'z') ? static_cast<char>(ch - 'a' + 'A') : ch;
}

Glyph glyph_for_char(const char ch) {
    switch (ch) {
        case 'A': return {{{0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}};
        case 'B': return {{{0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}}};
        case 'C': return {{{0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}}};
        case 'D': return {{{0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}}};
        case 'E': return {{{0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}}};
        case 'F': return {{{0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}}};
        case 'G': return {{{0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E}}};
        case 'H': return {{{0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}};
        case 'I': return {{{0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F}}};
        case 'J': return {{{0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E}}};
        case 'K': return {{{0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}};
        case 'L': return {{{0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}}};
        case 'M': return {{{0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}}};
        case 'N': return {{{0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}}};
        case 'O': return {{{0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}};
        case 'P': return {{{0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}}};
        case 'Q': return {{{0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}}};
        case 'R': return {{{0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}}};
        case 'S': return {{{0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}}};
        case 'T': return {{{0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}}};
        case 'U': return {{{0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}};
        case 'V': return {{{0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}}};
        case 'W': return {{{0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}}};
        case 'X': return {{{0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}}};
        case 'Y': return {{{0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}}};
        case 'Z': return {{{0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}}};
        case '0': return {{{0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}};
        case '1': return {{{0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}}};
        case '2': return {{{0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}};
        case '3': return {{{0x1E, 0x01, 0x01, 0x06, 0x01, 0x01, 0x1E}}};
        case '4': return {{{0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}};
        case '5': return {{{0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E}}};
        case '6': return {{{0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}};
        case '7': return {{{0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}}};
        case '8': return {{{0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}};
        case '9': return {{{0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}}};
        case ':': return {{{0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00}}};
        case '.': return {{{0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}}};
        case '-': return {{{0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}}};
        case '/': return {{{0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10}}};
        case '%': return {{{0x19, 0x19, 0x02, 0x04, 0x08, 0x13, 0x13}}};
        case '?': return {{{0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}}};
        case ' ': return {{{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}}};
        default: return glyph_for_char('?');
    }
}

void sanitize_text_copy(std::string_view input, char *output, const std::size_t output_size) {
    if (output == nullptr || output_size == 0) {
        return;
    }

    std::size_t index = 0;
    for (const unsigned char ch : input) {
        if (index >= output_size - 1 || index >= static_cast<std::size_t>(kMaxBodyChars)) {
            break;
        }
        if (ch == '\0') {
            break;
        }
        if (ch == '_') {
            output[index++] = ' ';
        } else if (std::isalnum(ch) != 0 || ch == ' ' || ch == ':' || ch == '.' || ch == '-' || ch == '/' || ch == '%') {
            output[index++] = static_cast<char>(std::toupper(ch));
        } else {
            output[index++] = '?';
        }
    }
    output[index] = '\0';
}

void format_age_copy(const std::int64_t timestamp_ms, const std::int64_t now_ms, char *output, const std::size_t output_size) {
    if (output == nullptr || output_size == 0) {
        return;
    }

    if (timestamp_ms <= 0 || now_ms <= timestamp_ms) {
        std::snprintf(output, output_size, "NEVER");
        return;
    }

    const auto seconds = static_cast<std::int64_t>((now_ms - timestamp_ms) / 1000);
    if (seconds < 60) {
        std::snprintf(output, output_size, "%lld SEC AGO", static_cast<long long>(seconds));
    } else if (seconds < 3600) {
        std::snprintf(output, output_size, "%lld MIN AGO", static_cast<long long>(seconds / 60));
    } else if (seconds < 86400) {
        std::snprintf(output, output_size, "%lld HR AGO", static_cast<long long>(seconds / 3600));
    } else {
        std::snprintf(output, output_size, "%lld DAY AGO", static_cast<long long>(seconds / 86400));
    }
}

std::uint16_t color_for_battery(const std::uint8_t percent, const bool has_reading) {
    if (!has_reading) {
        return kColorMuted;
    }
    if (percent <= 20) {
        return kColorError;
    }
    if (percent <= 50) {
        return kColorWarn;
    }
    return kColorOk;
}

std::uint16_t color_for_health(const BridgeHealth health) {
    switch (health) {
        case BridgeHealth::Ok:
            return kColorOk;
        case BridgeHealth::Busy:
            return kColorWarn;
        case BridgeHealth::Error:
            return kColorError;
    }

    return kColorText;
}

std::uint16_t color_for_zone(const ZoneRuntimeStatus status) {
    switch (status) {
        case ZoneRuntimeStatus::Running:
        case ZoneRuntimeStatus::Idle:
            return kColorOk;
        case ZoneRuntimeStatus::Starting:
        case ZoneRuntimeStatus::Stopping:
            return kColorWarn;
        case ZoneRuntimeStatus::Error:
            return kColorError;
        case ZoneRuntimeStatus::Disconnected:
        case ZoneRuntimeStatus::Unknown:
            return kColorMuted;
    }

    return kColorText;
}

void fill_buffer(std::uint16_t *buffer, const int pixel_count, const std::uint16_t color) {
    if (buffer == nullptr || pixel_count <= 0) {
        return;
    }
    for (int index = 0; index < pixel_count; ++index) {
        buffer[index] = color;
    }
}

void fill_rect_in_stripe(
    std::uint16_t *buffer,
    const int stripe_y,
    const int stripe_lines,
    const int x,
    const int y,
    const int width,
    const int height,
    const std::uint16_t color) {
    if (buffer == nullptr || width <= 0 || height <= 0 || stripe_lines <= 0) {
        return;
    }

    const auto x0 = std::max(0, x);
    const auto x1 = std::min(kDisplayWidth, x + width);
    const auto y0 = std::max(stripe_y, y);
    const auto y1 = std::min(stripe_y + stripe_lines, y + height);
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    for (int row = y0; row < y1; ++row) {
        const auto local_row = row - stripe_y;
        auto *row_ptr = buffer + local_row * kDisplayWidth;
        for (int col = x0; col < x1; ++col) {
            row_ptr[col] = color;
        }
    }
}

void draw_char_in_stripe(
    std::uint16_t *buffer,
    const int stripe_y,
    const int stripe_lines,
    const int x,
    const int y,
    char ch,
    const std::uint16_t fg,
    const std::uint16_t bg,
    const int scale) {
    const auto glyph = glyph_for_char(ascii_upper(ch));
    for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 5; ++col) {
            const auto on = (glyph.rows[row] & (1 << (4 - col))) != 0;
            fill_rect_in_stripe(buffer, stripe_y, stripe_lines, x + col * scale, y + row * scale, scale, scale, on ? fg : bg);
        }
    }
    fill_rect_in_stripe(buffer, stripe_y, stripe_lines, x + 5 * scale, y, scale, 7 * scale, bg);
}

void draw_text_in_stripe(
    std::uint16_t *buffer,
    const int stripe_y,
    const int stripe_lines,
    int x,
    const int y,
    const char *text,
    const std::uint16_t fg,
    const std::uint16_t bg,
    const int scale) {
    if (buffer == nullptr || text == nullptr) {
        return;
    }
    while (*text != '\0') {
        draw_char_in_stripe(buffer, stripe_y, stripe_lines, x, y, *text, fg, bg, scale);
        x += 6 * scale;
        ++text;
    }
}

struct RenderLine {
    char text[kMaxBodyChars + 1]{};
    std::uint16_t color{kColorText};
};

}  // namespace

StatusDisplay::~StatusDisplay() {
    if (transfer_buffer_ != nullptr) {
        heap_caps_free(transfer_buffer_);
        transfer_buffer_ = nullptr;
    }
}

esp_err_t StatusDisplay::init() {
    if (initialized_) {
        return ESP_OK;
    }

    transfer_buffer_ = static_cast<std::uint16_t *>(
        heap_caps_calloc(kTransferBufferPixels, sizeof(std::uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (transfer_buffer_ == nullptr) {
        ESP_LOGE(kTag, "failed to allocate AMOLED transfer buffer");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(kTag, "AMOLED transfer buffer allocated");

    if (s_last_render_stage != 0) {
        ESP_LOGW(kTag, "previous boot ended during display render stage=%d", s_last_render_stage);
        s_last_render_stage = 0;
    }

    if (!init_i2c_bus()) {
        return ESP_FAIL;
    }
    init_board_power_monitor();
    init_wake_inputs();
    if (!enable_panel_power()) {
        return ESP_FAIL;
    }
    if (!init_spi_bus()) {
        return ESP_FAIL;
    }
    if (!init_panel()) {
        return ESP_FAIL;
    }

    initialized_ = true;
    display_awake_ = false;
    force_render_ = true;
    display_awake_until_ms_ = 0;
    last_render_ms_ = 0;
    if (!set_panel_enabled(false)) {
        ESP_LOGW(kTag, "failed to put AMOLED panel into standby after init");
        display_awake_ = true;
        display_awake_until_ms_ = kDisplayWakeDurationMs;
    }
    ESP_LOGI(kTag, "AMOLED display init complete; press BOOT to restart, PWR to wake");
    return ESP_OK;
}

bool StatusDisplay::is_initialized() const {
    return initialized_;
}

void StatusDisplay::render(const DisplaySnapshot &snapshot, const std::int64_t now_ms) {
    if (!initialized_) {
        return;
    }

    update_display_power(now_ms);
    if (!display_awake_) {
        return;
    }
    if (!force_render_ && last_render_ms_ != 0 && (now_ms - last_render_ms_) < 1000) {
        return;
    }

    s_last_render_stage = 1;
    if (!first_render_done_) {
        ESP_LOGI(kTag, "render entry free_heap=%u", static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)));
    }

    if (!first_render_done_) {
        ESP_LOGI(kTag, "first render start");
    }

    maybe_refresh_board_power(now_ms);
    render_frame(snapshot, now_ms);

    if (!first_render_done_ && initialized_) {
        first_render_done_ = true;
        ESP_LOGI(kTag, "first render complete");
    }
    force_render_ = false;
    last_render_ms_ = now_ms;
    s_last_render_stage = 0;
}

bool StatusDisplay::init_wake_inputs() {
    if (wake_inputs_ready_) {
        return true;
    }

    gpio_config_t config = {};
    config.pin_bit_mask = (1ULL << kPinBootButton);
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    if (!check_result("boot_button_gpio_config", gpio_config(&config))) {
        return false;
    }

    boot_button_pressed_ = gpio_get_level(kPinBootButton) == 0;
    wake_inputs_ready_ = true;
    return true;
}

bool StatusDisplay::read_i2c_register(const std::uint8_t address, const std::uint8_t reg, std::uint8_t &value_out) {
    i2c_cmd_handle_t command = i2c_cmd_link_create();
    if (command == nullptr) {
        ESP_LOGE(kTag, "failed to allocate I2C command");
        return false;
    }

    i2c_master_start(command);
    i2c_master_write_byte(command, static_cast<std::uint8_t>((address << 1) | I2C_MASTER_WRITE), true);
    i2c_master_write_byte(command, reg, true);
    i2c_master_start(command);
    i2c_master_write_byte(command, static_cast<std::uint8_t>((address << 1) | I2C_MASTER_READ), true);
    i2c_master_read_byte(command, &value_out, I2C_MASTER_NACK);
    i2c_master_stop(command);

    const auto result = i2c_master_cmd_begin(kI2cHost, command, pdMS_TO_TICKS(250));
    i2c_cmd_link_delete(command);
    return result == ESP_OK;
}

bool StatusDisplay::write_i2c_register(const std::uint8_t address, const std::uint8_t reg, const std::uint8_t value) {
    i2c_cmd_handle_t command = i2c_cmd_link_create();
    if (command == nullptr) {
        ESP_LOGE(kTag, "failed to allocate I2C command");
        return false;
    }

    i2c_master_start(command);
    i2c_master_write_byte(command, static_cast<std::uint8_t>((address << 1) | I2C_MASTER_WRITE), true);
    i2c_master_write_byte(command, reg, true);
    i2c_master_write_byte(command, value, true);
    i2c_master_stop(command);

    const auto result = i2c_master_cmd_begin(kI2cHost, command, pdMS_TO_TICKS(250));
    i2c_cmd_link_delete(command);
    return result == ESP_OK;
}

bool StatusDisplay::init_board_power_monitor() {
    std::uint8_t chip_id = 0;
    if (!read_i2c_register(kPmuAddress, kPmuRegChipId, chip_id)) {
        ESP_LOGW(kTag, "AXP2101 PMU not responding on I2C");
        return false;
    }

    if (chip_id != kPmuChipId) {
        ESP_LOGW(kTag, "unexpected PMU chip id: 0x%02X", chip_id);
        return false;
    }

    std::uint8_t adc_ctrl = 0;
    if (!read_i2c_register(kPmuAddress, kPmuRegAdcChannelCtrl, adc_ctrl)) {
        ESP_LOGW(kTag, "failed to read AXP2101 ADC control");
        return false;
    }

    adc_ctrl = static_cast<std::uint8_t>(adc_ctrl | BIT(0) | BIT(2) | BIT(3));
    adc_ctrl = static_cast<std::uint8_t>(adc_ctrl & ~BIT(1));
    if (!write_i2c_register(kPmuAddress, kPmuRegAdcChannelCtrl, adc_ctrl)) {
        ESP_LOGW(kTag, "failed to configure AXP2101 ADC channels");
        return false;
    }

    std::uint8_t inten2 = 0;
    if (read_i2c_register(kPmuAddress, kPmuRegIntEn2, inten2)) {
        inten2 = static_cast<std::uint8_t>(inten2 | kPmuPkeyShortIrqMask);
        if (!write_i2c_register(kPmuAddress, kPmuRegIntEn2, inten2)) {
            ESP_LOGW(kTag, "failed to enable AXP2101 key IRQs");
        }
        write_i2c_register(kPmuAddress, kPmuRegIntStatus2, 0xFF);
    }

    pmu_ready_ = true;
    ESP_LOGI(kTag, "AXP2101 PMU ready");
    return true;
}

bool StatusDisplay::refresh_board_power(const std::int64_t now_ms) {
    if (!pmu_ready_) {
        return false;
    }

    std::uint8_t status1 = 0;
    std::uint8_t status2 = 0;
    std::uint8_t percent = 0;
    std::uint8_t batt_high = 0;
    std::uint8_t batt_low = 0;
    std::uint8_t sys_high = 0;
    std::uint8_t sys_low = 0;

    if (!read_i2c_register(kPmuAddress, kPmuRegStatus1, status1) || !read_i2c_register(kPmuAddress, kPmuRegStatus2, status2) ||
        !read_i2c_register(kPmuAddress, kPmuRegBattPercent, percent) ||
        !read_i2c_register(kPmuAddress, kPmuRegBattVoltageHigh, batt_high) ||
        !read_i2c_register(kPmuAddress, kPmuRegBattVoltageLow, batt_low) ||
        !read_i2c_register(kPmuAddress, kPmuRegSystemVoltageHigh, sys_high) ||
        !read_i2c_register(kPmuAddress, kPmuRegSystemVoltageLow, sys_low)) {
        ESP_LOGW(kTag, "AXP2101 board power read failed");
        return false;
    }

    board_battery_present_ = (status1 & BIT(3)) != 0;
    board_battery_charging_ = ((status2 >> 5) & 0x07) == 0x01;
    board_usb_present_ = (status2 & BIT(3)) == 0;
    board_battery_percent_ = board_battery_present_ ? percent : 0;
    board_battery_mv_ = board_battery_present_
                            ? static_cast<std::uint16_t>(((batt_high & 0x1Fu) << 8u) | batt_low)
                            : 0;
    board_system_mv_ = static_cast<std::uint16_t>(((sys_high & 0x3Fu) << 8u) | sys_low);
    board_power_last_update_ms_ = now_ms;
    return true;
}

void StatusDisplay::maybe_refresh_board_power(const std::int64_t now_ms) {
    if (!pmu_ready_) {
        return;
    }
    if (board_power_last_update_ms_ != 0 && (now_ms - board_power_last_update_ms_) < kBoardPowerRefreshMs) {
        return;
    }
    refresh_board_power(now_ms);
}

bool StatusDisplay::poll_power_key_wake_request() {
    if (!pmu_ready_) {
        return false;
    }

    std::uint8_t irq2 = 0;
    if (!read_i2c_register(kPmuAddress, kPmuRegIntStatus2, irq2)) {
        return false;
    }

    const bool short_press = (irq2 & kPmuPkeyShortIrqMask) != 0;
    if (irq2 != 0) {
        write_i2c_register(kPmuAddress, kPmuRegIntStatus2, irq2);
    }
    return short_press;
}

void StatusDisplay::wake_display(const std::int64_t now_ms) {
    display_awake_until_ms_ = now_ms + kDisplayWakeDurationMs;
    if (!display_awake_) {
        display_awake_ = true;
        set_panel_enabled(true);
        force_render_ = true;
        last_render_ms_ = 0;
        ESP_LOGI(kTag, "Display wake");
    }
}

void StatusDisplay::sleep_display() {
    if (!display_awake_) {
        return;
    }
    if (set_panel_enabled(false)) {
        display_awake_ = false;
        ESP_LOGI(kTag, "Display sleep");
    }
}

void StatusDisplay::update_display_power(const std::int64_t now_ms) {
    if (wake_inputs_ready_) {
        const bool boot_pressed = gpio_get_level(kPinBootButton) == 0;
        if (boot_pressed && !boot_button_pressed_) {
            boot_restart_armed_ = true;
        } else if (!boot_pressed && boot_button_pressed_ && boot_restart_armed_) {
            boot_restart_armed_ = false;
            ESP_LOGW(kTag, "BOOT released; restarting");
            vTaskDelay(pdMS_TO_TICKS(50));
            esp_restart();
        }
        boot_button_pressed_ = boot_pressed;
    }

    if (poll_power_key_wake_request()) {
        wake_display(now_ms);
    }

    if (display_awake_ && now_ms >= display_awake_until_ms_) {
        sleep_display();
    }
}

bool StatusDisplay::init_i2c_bus() {
    if (i2c_ready_) {
        return true;
    }

    i2c_config_t config = {};
    config.mode = I2C_MODE_MASTER;
    config.sda_io_num = kPinTouchSda;
    config.sda_pullup_en = GPIO_PULLUP_ENABLE;
    config.scl_io_num = kPinTouchScl;
    config.scl_pullup_en = GPIO_PULLUP_ENABLE;
    config.master.clk_speed = 200 * 1000;
    config.clk_flags = 0;

    if (!check_result("i2c_param_config", i2c_param_config(kI2cHost, &config))) {
        return false;
    }

    const auto install_result = i2c_driver_install(kI2cHost, config.mode, 0, 0, 0);
    if (install_result != ESP_OK && install_result != ESP_ERR_INVALID_STATE) {
        return check_result("i2c_driver_install", install_result);
    }

    i2c_ready_ = true;
    ESP_LOGI(kTag, "AMOLED I2C bus ready");
    return true;
}

bool StatusDisplay::enable_panel_power() {
    auto write_reg = [](const std::uint8_t reg, const std::uint8_t value) -> bool {
        i2c_cmd_handle_t command = i2c_cmd_link_create();
        if (command == nullptr) {
            ESP_LOGE(kTag, "failed to allocate I2C command");
            return false;
        }
        i2c_master_start(command);
        i2c_master_write_byte(command, static_cast<std::uint8_t>((kIoExpanderAddress << 1) | I2C_MASTER_WRITE), true);
        i2c_master_write_byte(command, reg, true);
        i2c_master_write_byte(command, value, true);
        i2c_master_stop(command);
        const auto result = i2c_master_cmd_begin(kI2cHost, command, pdMS_TO_TICKS(250));
        i2c_cmd_link_delete(command);
        return check_result("i2c_master_cmd_begin", result);
    };

    ESP_LOGI(kTag, "AMOLED power-on via TCA9554");
    if (!write_reg(kIoExpanderConfigReg, kIoExpanderConfigValue)) {
        return false;
    }
    if (!write_reg(kIoExpanderOutputReg, 0x00)) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    if (!write_reg(kIoExpanderOutputReg, kIoExpanderDisplayMask)) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(120));
    return true;
}

bool StatusDisplay::init_spi_bus() {
    if (spi_ready_) {
        return true;
    }

    spi_bus_config_t bus_config = {};
    bus_config.sclk_io_num = kPinLcdPclk;
    bus_config.data0_io_num = kPinLcdData0;
    bus_config.data1_io_num = kPinLcdData1;
    bus_config.data2_io_num = kPinLcdData2;
    bus_config.data3_io_num = kPinLcdData3;
    bus_config.max_transfer_sz = kTransferBufferPixels * sizeof(std::uint16_t);
    const auto bus_result = spi_bus_initialize(kSpiHost, &bus_config, SPI_DMA_CH_AUTO);
    if (bus_result != ESP_OK && bus_result != ESP_ERR_INVALID_STATE) {
        return check_result("spi_bus_initialize", bus_result);
    }

    spi_ready_ = true;
    ESP_LOGI(kTag, "AMOLED QSPI bus ready");
    return true;
}

bool StatusDisplay::init_panel() {
    if (panel_ != nullptr && panel_io_ != nullptr) {
        return true;
    }

    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = kPinLcdCs;
    io_config.dc_gpio_num = -1;
    io_config.spi_mode = 0;
    io_config.pclk_hz = 40 * 1000 * 1000;
    io_config.trans_queue_depth = 1;
    io_config.on_color_trans_done = nullptr;
    io_config.user_ctx = nullptr;
    io_config.lcd_cmd_bits = 32;
    io_config.lcd_param_bits = 8;
    io_config.flags.quad_mode = true;

    esp_lcd_panel_io_handle_t io_handle = nullptr;
    if (!check_result("esp_lcd_new_panel_io_spi",
                      esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(kSpiHost), &io_config, &io_handle))) {
        return false;
    }

    sh8601_vendor_config_t vendor_config = {};
    vendor_config.init_cmds = kLcdInitCmds;
    vendor_config.init_cmds_size = sizeof(kLcdInitCmds) / sizeof(kLcdInitCmds[0]);
    vendor_config.flags.use_qspi_interface = 1;

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = -1;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    panel_config.vendor_config = &vendor_config;

    esp_lcd_panel_handle_t panel_handle = nullptr;
    if (!check_result("esp_lcd_new_panel_sh8601", esp_lcd_new_panel_sh8601(io_handle, &panel_config, &panel_handle)) ||
        !check_result("esp_lcd_panel_reset", esp_lcd_panel_reset(panel_handle)) ||
        !check_result("esp_lcd_panel_init", esp_lcd_panel_init(panel_handle)) ||
        !check_result("esp_lcd_panel_mirror", esp_lcd_panel_mirror(panel_handle, false, false)) ||
        !check_result("esp_lcd_panel_disp_on_off", esp_lcd_panel_disp_on_off(panel_handle, true))) {
        return false;
    }

    panel_io_ = io_handle;
    panel_ = panel_handle;
    ESP_LOGI(kTag, "SH8601 panel ready");
    return true;
}

bool StatusDisplay::set_panel_enabled(const bool enabled) {
    if (panel_ == nullptr) {
        return false;
    }
    return check_result(
        enabled ? "esp_lcd_panel_disp_on" : "esp_lcd_panel_disp_off",
        esp_lcd_panel_disp_on_off(static_cast<esp_lcd_panel_handle_t>(panel_), enabled));
}

bool StatusDisplay::wait_for_panel_idle() {
    if (panel_io_ == nullptr) {
        return false;
    }
    return check_result(
        "panel_idle_wait",
        esp_lcd_panel_io_tx_param(static_cast<esp_lcd_panel_io_handle_t>(panel_io_), -1, nullptr, 0));
}

void StatusDisplay::render_solid_color(const std::uint16_t color) {
    if (transfer_buffer_ == nullptr || panel_ == nullptr) {
        return;
    }

    for (int y = 0; y < kDisplayHeight; y += kTransferLines) {
        fill_buffer(transfer_buffer_, kTransferBufferPixels, color);
        const auto draw_result = esp_lcd_panel_draw_bitmap(
            static_cast<esp_lcd_panel_handle_t>(panel_),
            0,
            y,
            kDisplayWidth,
            y + kTransferLines,
            transfer_buffer_);
        if (!check_result("esp_lcd_panel_draw_bitmap", draw_result) || !wait_for_panel_idle()) {
            initialized_ = false;
            ESP_LOGE(kTag, "display disabled after solid-color flush failure");
            return;
        }
    }
}

void StatusDisplay::render_frame(const DisplaySnapshot &snapshot, const std::int64_t now_ms) {
    std::array<RenderLine, 2> connectivity_lines{};
    std::array<RenderLine, 4> hunter_lines{};
    std::array<RenderLine, 4> health_lines{};
    std::size_t connectivity_count = 0;
    std::size_t hunter_count = 0;
    std::size_t health_count = 0;
    auto add_line = [](auto &lines, std::size_t &count, const std::uint16_t color, const char *text) {
        if (count >= lines.size() || text == nullptr) {
            return;
        }
        lines[count].color = color;
        std::snprintf(lines[count].text, sizeof(lines[count].text), "%.*s", kMaxBodyChars, text);
        ++count;
    };

    char sanitized[48] = {};
    char sanitized2[48] = {};
    char sanitized3[48] = {};
    char age[16] = {};
    char line[64] = {};

    sanitize_text_copy(snapshot.state.bridge_error.data(), sanitized, sizeof(sanitized));
    const auto health =
        snapshot.bridge_busy ? BridgeHealth::Busy : (sanitized[0] != '\0' ? BridgeHealth::Error : BridgeHealth::Ok);

    sanitize_text_copy(snapshot.ble.last_status.data(), sanitized3, sizeof(sanitized3));
    if (sanitized3[0] == '\0') {
        std::snprintf(sanitized3, sizeof(sanitized3), "IDLE");
    }

    const auto mqtt_color = snapshot.mqtt_connected ? kColorOk : (snapshot.wifi_connected ? kColorWarn : kColorMuted);
    format_age_copy(snapshot.mqtt_last_change_ms, now_ms, age, sizeof(age));
    if (snapshot.mqtt_last_change_ms > 0) {
        std::snprintf(line, sizeof(line), "MQTT: %s - %s", snapshot.mqtt_connected ? "UP" : "DOWN", age);
    } else {
        std::snprintf(line, sizeof(line), "MQTT: %s", snapshot.mqtt_connected ? "UP" : "DOWN");
    }
    add_line(connectivity_lines, connectivity_count, mqtt_color, line);

    sanitize_text_copy(snapshot.wifi_ip.data(), sanitized, sizeof(sanitized));
    if (snapshot.wifi_connected && sanitized[0] != '\0' && std::strcmp(sanitized, "-") != 0) {
        std::snprintf(line, sizeof(line), "WIFI: UP - %s", sanitized);
    } else if (snapshot.wifi_connected) {
        std::snprintf(line, sizeof(line), "WIFI: UP");
    } else {
        std::snprintf(line, sizeof(line), "WIFI: DOWN");
    }
    add_line(connectivity_lines, connectivity_count, snapshot.wifi_connected ? kColorOk : kColorError, line);

    format_age_copy(snapshot.state.battery_updated_epoch_ms, now_ms, age, sizeof(age));
    if (snapshot.state.battery_updated_epoch_ms > 0) {
        std::snprintf(line, sizeof(line), "BATTERY: %u%% - %s", snapshot.state.battery_percent, age);
        add_line(hunter_lines, hunter_count, color_for_battery(snapshot.state.battery_percent, true), line);
    } else {
        std::snprintf(line, sizeof(line), "BATTERY: NEVER READ");
        add_line(hunter_lines, hunter_count, kColorMuted, line);
    }

    for (std::size_t index = 0; index < kZoneCount; ++index) {
        sanitize_text_copy(to_string(snapshot.state.zones[index].runtime_status), sanitized2, sizeof(sanitized2));
        if (snapshot.state.zones[index].remaining_seconds > 0) {
            std::snprintf(
                line,
                sizeof(line),
                "ZONE %u: %s %us",
                static_cast<unsigned>(index + 1),
                sanitized2,
                static_cast<unsigned>(snapshot.state.zones[index].remaining_seconds));
        } else {
            std::snprintf(line, sizeof(line), "ZONE %u: %s", static_cast<unsigned>(index + 1), sanitized2);
        }
        add_line(hunter_lines, hunter_count, color_for_zone(snapshot.state.zones[index].runtime_status), line);
    }

    format_age_copy(snapshot.ble.last_success_ms, now_ms, age, sizeof(age));
    if (snapshot.ble.last_success_ms > 0) {
        std::snprintf(line, sizeof(line), "BLE LINK: OK - %s", age);
        add_line(hunter_lines, hunter_count, kColorOk, line);
    } else {
        std::snprintf(line, sizeof(line), "BLE LINK: NEVER");
        add_line(hunter_lines, hunter_count, kColorMuted, line);
    }

    if (!pmu_ready_) {
        std::snprintf(line, sizeof(line), "BOARD: PMU OFFLINE");
        add_line(health_lines, health_count, kColorMuted, line);
    } else if (!board_battery_present_) {
        std::snprintf(line, sizeof(line), "BOARD: USB - %u MV", static_cast<unsigned>(board_system_mv_));
        add_line(health_lines, health_count, board_usb_present_ ? kColorOk : kColorMuted, line);
    } else {
        std::snprintf(
            line,
            sizeof(line),
            "BOARD: %u%% %s",
            static_cast<unsigned>(board_battery_percent_),
            board_battery_charging_ ? "CHARGING" : "BATTERY");
        add_line(health_lines, health_count, color_for_battery(board_battery_percent_, true), line);
    }

    sanitize_text_copy(to_string(health), sanitized, sizeof(sanitized));
    std::snprintf(line, sizeof(line), "HEALTH: %s", sanitized);
    add_line(health_lines, health_count, color_for_health(health), line);

    format_age_copy(snapshot.ble.last_attempt_ms, now_ms, age, sizeof(age));
    if (snapshot.ble.last_attempt_ms > 0) {
        std::snprintf(line, sizeof(line), "LAST TRY: %s", age);
    } else {
        std::snprintf(line, sizeof(line), "LAST TRY: NEVER");
    }
    add_line(health_lines, health_count, kColorMuted, line);

    if (snapshot.state.bridge_error[0] != '\0') {
        sanitize_text_copy(snapshot.state.bridge_error.data(), sanitized, sizeof(sanitized));
        std::snprintf(line, sizeof(line), "ERROR: %s", sanitized);
        add_line(health_lines, health_count, kColorError, line);
    } else {
        const auto ble_color =
            (std::strstr(sanitized3, "FAIL") != nullptr || std::strstr(sanitized3, "TIMEOUT") != nullptr ||
             std::strstr(sanitized3, "ERROR") != nullptr || std::strstr(sanitized3, "DISCONNECT") != nullptr)
                ? kColorError
                : (std::strstr(sanitized3, "IDLE") != nullptr ? kColorMuted : kColorWarn);
        std::snprintf(line, sizeof(line), "BLE: %s", sanitized3);
        add_line(health_lines, health_count, ble_color, line);
    }

    for (int stripe_y = 0; stripe_y < kDisplayHeight; stripe_y += kTransferLines) {
        s_last_render_stage = 2;
        fill_buffer(transfer_buffer_, kTransferBufferPixels, kColorBackground);

        s_last_render_stage = 3;
        draw_text_in_stripe(
            transfer_buffer_,
            stripe_y,
            kTransferLines,
            kOuterPad,
            kHeaderY,
            "HUNTER BRIDGE",
            kColorText,
            kColorBackground,
            kHeaderScale);
        fill_rect_in_stripe(transfer_buffer_, stripe_y, kTransferLines, kOuterPad, 58, kDisplayWidth - (kOuterPad * 2), 1, kColorMuted);
        fill_rect_in_stripe(transfer_buffer_, stripe_y, kTransferLines, kOuterPad, 58, 118, 3, kColorAccent);

        s_last_render_stage = 4;
        const auto draw_section = [&](const int title_y,
                                      const int underline_y,
                                      const char *title,
                                      const auto &lines,
                                      const std::size_t line_count,
                                      const std::array<int, 4> &line_positions) {
            draw_text_in_stripe(
                transfer_buffer_,
                stripe_y,
                kTransferLines,
                kOuterPad,
                title_y,
                title,
                kColorAccent,
                kColorBackground,
                kSectionScale);
            fill_rect_in_stripe(
                transfer_buffer_,
                stripe_y,
                kTransferLines,
                kOuterPad,
                underline_y,
                kDisplayWidth - (kOuterPad * 2),
                1,
                kColorMuted);
            fill_rect_in_stripe(transfer_buffer_, stripe_y, kTransferLines, kOuterPad, underline_y, 92, 2, kColorAccent);
            for (std::size_t index = 0; index < line_count && index < line_positions.size(); ++index) {
                draw_text_in_stripe(
                    transfer_buffer_,
                    stripe_y,
                    kTransferLines,
                    kOuterPad,
                    line_positions[index],
                    lines[index].text,
                    lines[index].color,
                    kColorBackground,
                    kBodyScale);
            }
        };

        draw_section(
            kSectionTitleY,
            kSectionTitleY + 18,
            "CONNECTIVITY",
            connectivity_lines,
            connectivity_count,
            {kSectionLineY1, kSectionLineY2, 0, 0});

        draw_section(
            kSection2TitleY,
            kSection2TitleY + 18,
            "HUNTER BTT",
            hunter_lines,
            hunter_count,
            {kSection2LineY1, kSection2LineY2, kSection2LineY3, kSection2LineY4});

        draw_section(
            kSection3TitleY,
            kSection3TitleY + 18,
            "HEALTH",
            health_lines,
            health_count,
            {kSection3LineY1, kSection3LineY2, kSection3LineY3, kSection3LineY4});

        s_last_render_stage = 5;
        const auto draw_result = esp_lcd_panel_draw_bitmap(
            static_cast<esp_lcd_panel_handle_t>(panel_),
            0,
            stripe_y,
            kDisplayWidth,
            stripe_y + kTransferLines,
            transfer_buffer_);
        if (!check_result("esp_lcd_panel_draw_bitmap", draw_result) || !wait_for_panel_idle()) {
            initialized_ = false;
            ESP_LOGE(kTag, "display disabled after frame flush failure");
            return;
        }
        taskYIELD();
    }
}

}  // namespace bridge

#include "ble_transport.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <utility>

extern "C" {
#include "esp_nimble_hci.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
}

#include "bridge_config.h"
#include "hunter_protocol.h"

namespace bridge {

namespace {

constexpr char kTag[] = "BleTransport";
constexpr std::uint32_t kScanTimeoutMs = 5000;
constexpr std::uint16_t kCustomServiceUuid16 = 0xFF80;
constexpr std::uint16_t kBatteryServiceUuid16 = 0x180F;

std::string normalize_mac(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

std::vector<std::uint8_t> mbuf_to_vector(os_mbuf *buffer) {
    std::vector<std::uint8_t> output;
    if (buffer == nullptr) {
        return output;
    }
    output.resize(OS_MBUF_PKTLEN(buffer));
    std::uint16_t out_len = 0;
    ble_hs_mbuf_to_flat(buffer, output.data(), output.size(), &out_len);
    output.resize(out_len);
    return output;
}

}  // namespace

BleTransport *BleTransport::instance_ = nullptr;

BleTransport::BleTransport() : target_mac_(normalize_mac(config::kHunterMac)) {
    instance_ = this;
}

BleTransport::~BleTransport() {
    disconnect();
    if (scan_done_sem_ != nullptr) {
        vSemaphoreDelete(static_cast<SemaphoreHandle_t>(scan_done_sem_));
        scan_done_sem_ = nullptr;
    }
    if (connect_sem_ != nullptr) {
        vSemaphoreDelete(static_cast<SemaphoreHandle_t>(connect_sem_));
        connect_sem_ = nullptr;
    }
    if (gatt_sem_ != nullptr) {
        vSemaphoreDelete(static_cast<SemaphoreHandle_t>(gatt_sem_));
        gatt_sem_ = nullptr;
    }
    if (notify_queue_ != nullptr) {
        vQueueDelete(static_cast<QueueHandle_t>(notify_queue_));
        notify_queue_ = nullptr;
    }
    if (mutex_ != nullptr) {
        vSemaphoreDelete(static_cast<SemaphoreHandle_t>(mutex_));
        mutex_ = nullptr;
    }
}

esp_err_t BleTransport::init() {
    if (initialized_) {
        return ESP_OK;
    }

    scan_done_sem_ = xSemaphoreCreateBinary();
    connect_sem_ = xSemaphoreCreateBinary();
    gatt_sem_ = xSemaphoreCreateBinary();
    notify_queue_ = xQueueCreate(16, sizeof(RawNotification));
    mutex_ = xSemaphoreCreateMutex();
    if (scan_done_sem_ == nullptr || connect_sem_ == nullptr || gatt_sem_ == nullptr || notify_queue_ == nullptr || mutex_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_nimble_hci_and_controller_init());
    nimble_port_init();
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_hs_cfg.sync_cb = &BleTransport::on_sync;
    ble_hs_cfg.reset_cb = &BleTransport::on_reset;
    nimble_port_freertos_init(&BleTransport::host_task);
    initialized_ = true;
    return ESP_OK;
}

bool BleTransport::ensure_ble_ready() {
    if (!initialized_ && init() != ESP_OK) {
        set_error("ble init failed");
        return false;
    }

    const auto start = esp_timer_get_time();
    while (!synced_) {
        if ((esp_timer_get_time() - start) / 1000 > kConnectTimeoutMs) {
            set_error("nimble sync timeout");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return true;
}

bool BleTransport::scan_for_target() {
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(scan_done_sem_), 0);
    target_found_ = false;

    ble_gap_disc_params params{};
    params.itvl = 0x0010;
    params.window = 0x0010;
    params.passive = 0;
    params.filter_duplicates = 1;

    const auto rc = ble_gap_disc(own_addr_type_, static_cast<int32_t>(kScanTimeoutMs), &params, &BleTransport::gap_event, this);
    if (rc != 0) {
        set_error("scan start failed");
        return false;
    }

    if (xSemaphoreTake(static_cast<SemaphoreHandle_t>(scan_done_sem_), pdMS_TO_TICKS(kScanTimeoutMs + 1000U)) != pdTRUE) {
        set_error("scan timeout");
        ble_gap_disc_cancel();
        return false;
    }

    if (!target_found_) {
        set_error("target MAC not found during scan");
        return false;
    }
    return true;
}

bool BleTransport::connect_once() {
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(connect_sem_), 0);
    const auto rc = ble_gap_connect(
        own_addr_type_,
        &peer_addr_,
        static_cast<int32_t>(kConnectTimeoutMs),
        nullptr,
        &BleTransport::gap_event,
        this);
    if (rc != 0) {
        set_error("ble_gap_connect failed");
        return false;
    }

    if (xSemaphoreTake(static_cast<SemaphoreHandle_t>(connect_sem_), pdMS_TO_TICKS(kConnectTimeoutMs + 1000U)) != pdTRUE) {
        if (conn_handle_ != 0xFFFF) {
            ble_gap_terminate(conn_handle_, BLE_ERR_REM_USER_CONN_TERM);
        }
        set_error("connect timeout");
        return false;
    }

    return conn_handle_ != 0xFFFF;
}

bool BleTransport::connect() {
    if (xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), pdMS_TO_TICKS(kConnectTimeoutMs)) != pdTRUE) {
        set_error("transport mutex timeout");
        return false;
    }

    bool success = false;
    if (conn_handle_ != 0xFFFF) {
        success = true;
    } else if (ensure_ble_ready()) {
        clear_notifications();
        for (int attempt = 0; attempt < 2 && !success; ++attempt) {
            if (!scan_for_target()) {
                continue;
            }
            if (!connect_once()) {
                continue;
            }
            if (!discover_handles()) {
                disconnect();
                continue;
            }
            success = true;
        }
    }

    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return success;
}

void BleTransport::disconnect() {
    if (conn_handle_ != 0xFFFF) {
        ble_gap_terminate(conn_handle_, BLE_ERR_REM_USER_CONN_TERM);
        conn_handle_ = 0xFFFF;
    }
    reset_handles();
}

bool BleTransport::is_connected() const {
    return conn_handle_ != 0xFFFF;
}

bool BleTransport::discover_service_range(const std::uint16_t service_uuid, std::uint16_t &start, std::uint16_t &end) {
    pending_service_start_ = 0;
    pending_service_end_ = 0;
    service_found_ = false;
    last_status_ = 0;
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(gatt_sem_), 0);

    auto uuid = BLE_UUID16_INIT(service_uuid);
    const auto rc = ble_gattc_disc_svc_by_uuid(conn_handle_, &uuid.u, &BleTransport::service_discovery_cb, this);
    if (rc != 0) {
        set_error("service discovery call failed");
        return false;
    }

    if (xSemaphoreTake(static_cast<SemaphoreHandle_t>(gatt_sem_), pdMS_TO_TICKS(kGattTimeoutMs)) != pdTRUE) {
        set_error("service discovery timeout");
        return false;
    }

    if (!service_found_ || last_status_ != 0) {
        set_error("service discovery incomplete");
        return false;
    }

    start = pending_service_start_;
    end = pending_service_end_;
    return true;
}

bool BleTransport::discover_characteristics(const std::uint16_t start, const std::uint16_t end) {
    last_status_ = 0;
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(gatt_sem_), 0);
    const auto rc = ble_gattc_disc_all_chrs(conn_handle_, start, end, &BleTransport::characteristic_discovery_cb, this);
    if (rc != 0) {
        set_error("characteristic discovery call failed");
        return false;
    }

    if (xSemaphoreTake(static_cast<SemaphoreHandle_t>(gatt_sem_), pdMS_TO_TICKS(kGattTimeoutMs)) != pdTRUE) {
        set_error("characteristic discovery timeout");
        return false;
    }

    if (last_status_ != 0) {
        set_error("characteristic discovery incomplete");
        return false;
    }
    return true;
}

bool BleTransport::discover_handles() {
    reset_handles();

    std::uint16_t custom_start = 0;
    std::uint16_t custom_end = 0;
    if (!discover_service_range(kCustomServiceUuid16, custom_start, custom_end)) {
        return false;
    }
    if (!discover_characteristics(custom_start, custom_end)) {
        return false;
    }

    std::uint16_t battery_start = 0;
    std::uint16_t battery_end = 0;
    if (!discover_service_range(kBatteryServiceUuid16, battery_start, battery_end)) {
        return false;
    }
    if (!discover_characteristics(battery_start, battery_end)) {
        return false;
    }

    if (handles_.ff82 == 0 || handles_.ff83 == 0 || handles_.ff86 == 0 || handles_.ff8a == 0 || handles_.ff8b == 0 || handles_.ff8f == 0 || handles_.battery_level == 0) {
        set_error("required characteristic handles missing");
        return false;
    }

    return enable_notification(handles_.ff82) && enable_notification(handles_.ff8a) && enable_notification(handles_.ff8f);
}

bool BleTransport::enable_notification(const std::uint16_t value_handle) {
    const std::array<std::uint8_t, 2> cccd_on{0x01, 0x00};
    return write_handle(static_cast<std::uint16_t>(value_handle + 1U), cccd_on.data(), cccd_on.size());
}

bool BleTransport::read_handle(const std::uint16_t handle, std::vector<std::uint8_t> &out) {
    if (conn_handle_ == 0xFFFF || handle == 0) {
        set_error("read requested without active connection");
        return false;
    }
    last_read_payload_.clear();
    last_status_ = 0;
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(gatt_sem_), 0);

    const auto rc = ble_gattc_read(conn_handle_, handle, &BleTransport::read_cb, this);
    if (rc != 0) {
        set_error("read call failed");
        return false;
    }

    if (xSemaphoreTake(static_cast<SemaphoreHandle_t>(gatt_sem_), pdMS_TO_TICKS(kGattTimeoutMs)) != pdTRUE) {
        set_error("read timeout");
        return false;
    }

    if (last_status_ != 0) {
        set_error("read failed");
        return false;
    }

    out = last_read_payload_;
    return true;
}

bool BleTransport::write_handle(const std::uint16_t handle, const std::uint8_t *data, const std::size_t len) {
    if (conn_handle_ == 0xFFFF || handle == 0) {
        set_error("write requested without active connection");
        return false;
    }
    last_status_ = 0;
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(gatt_sem_), 0);

    const auto rc = ble_gattc_write_flat(conn_handle_, handle, data, len, &BleTransport::write_cb, this);
    if (rc != 0) {
        set_error("write call failed");
        return false;
    }

    if (xSemaphoreTake(static_cast<SemaphoreHandle_t>(gatt_sem_), pdMS_TO_TICKS(kGattTimeoutMs)) != pdTRUE) {
        set_error("write timeout");
        return false;
    }

    if (last_status_ != 0) {
        set_error("write failed");
        return false;
    }

    return true;
}

std::uint16_t BleTransport::handle_for_uuid(const char *uuid) const {
    const std::string value(uuid);
    if (value == protocol::kFf81Uuid) return handles_.ff81;
    if (value == protocol::kFf82Uuid) return handles_.ff82;
    if (value == protocol::kFf83Uuid) return handles_.ff83;
    if (value == protocol::kFf84Uuid) return handles_.ff84;
    if (value == protocol::kFf86Uuid) return handles_.ff86;
    if (value == protocol::kFf87Uuid) return handles_.ff87;
    if (value == protocol::kFf88Uuid) return handles_.ff88;
    if (value == protocol::kFf89Uuid) return handles_.ff89;
    if (value == protocol::kFf8aUuid) return handles_.ff8a;
    if (value == protocol::kFf8bUuid) return handles_.ff8b;
    if (value == protocol::kFf8cUuid) return handles_.ff8c;
    if (value == protocol::kFf8dUuid) return handles_.ff8d;
    if (value == protocol::kFf8eUuid) return handles_.ff8e;
    if (value == protocol::kFf8fUuid) return handles_.ff8f;
    if (value == protocol::kBatteryLevelUuid) return handles_.battery_level;
    return 0;
}

std::string BleTransport::uuid_for_handle(const std::uint16_t handle) const {
    if (handle == handles_.ff82) return protocol::kFf82Uuid;
    if (handle == handles_.ff8a) return protocol::kFf8aUuid;
    if (handle == handles_.ff8f) return protocol::kFf8fUuid;
    if (handle == handles_.battery_level) return protocol::kBatteryLevelUuid;
    return "";
}

bool BleTransport::read_characteristic(const char *uuid, std::vector<std::uint8_t> &out) {
    const auto handle = handle_for_uuid(uuid);
    if (handle == 0) {
        set_error("unknown characteristic");
        return false;
    }
    return read_handle(handle, out);
}

bool BleTransport::write_characteristic(const char *uuid, const std::uint8_t *data, const std::size_t len) {
    const auto handle = handle_for_uuid(uuid);
    if (handle == 0) {
        set_error("unknown characteristic");
        return false;
    }
    return write_handle(handle, data, len);
}

bool BleTransport::write_characteristic(const char *uuid, const std::vector<std::uint8_t> &data) {
    return write_characteristic(uuid, data.data(), data.size());
}

bool BleTransport::read_battery_percent(std::uint8_t &percent_out) {
    std::vector<std::uint8_t> payload;
    if (!read_handle(handles_.battery_level, payload) || payload.empty()) {
        return false;
    }
    percent_out = payload[0];
    return true;
}

void BleTransport::clear_notifications() {
    RawNotification ignored{};
    while (xQueueReceive(static_cast<QueueHandle_t>(notify_queue_), &ignored, 0) == pdTRUE) {
    }
}

bool BleTransport::wait_for_notification(NotificationEvent &event, const std::uint32_t timeout_ms) {
    RawNotification raw{};
    if (xQueueReceive(static_cast<QueueHandle_t>(notify_queue_), &raw, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return false;
    }

    event.uuid = uuid_for_handle(raw.attr_handle);
    event.payload_len = raw.payload_len;
    std::copy_n(raw.payload, raw.payload_len, event.payload.begin());
    event.hex = protocol::bytes_to_hex(raw.payload, raw.payload_len);
    return true;
}

void BleTransport::set_disconnect_callback(std::function<void(int)> callback) {
    disconnect_callback_ = std::move(callback);
}

const std::string &BleTransport::last_error() const {
    return last_error_;
}

void BleTransport::set_error(std::string message) {
    last_error_ = std::move(message);
    ESP_LOGW(kTag, "%s", last_error_.c_str());
}

void BleTransport::reset_handles() {
    handles_ = GattHandles{};
}

void BleTransport::host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void BleTransport::on_reset(const int reason) {
    if (instance_ != nullptr) {
        instance_->set_error("nimble reset");
        instance_->synced_ = false;
    }
    ESP_LOGW(kTag, "nimble reset reason=%d", reason);
}

void BleTransport::on_sync() {
    if (instance_ == nullptr) {
        return;
    }

    ble_hs_id_infer_auto(0, &instance_->own_addr_type_);
    instance_->synced_ = true;
    ESP_LOGI(kTag, "nimble synced");
}

int BleTransport::gap_event(struct ble_gap_event *event, void *arg) {
    auto *self = static_cast<BleTransport *>(arg);
    if (self == nullptr) {
        return 0;
    }

    switch (event->type) {
        case BLE_GAP_EVENT_DISC: {
            char address_buffer[BLE_ADDR_STR_LEN] = {};
            ble_addr_to_str(&event->disc.addr, address_buffer);
            if (normalize_mac(address_buffer) == self->target_mac_) {
                self->peer_addr_ = event->disc.addr;
                self->target_found_ = true;
                ble_gap_disc_cancel();
            }
            return 0;
        }
        case BLE_GAP_EVENT_DISC_COMPLETE:
            xSemaphoreGive(static_cast<SemaphoreHandle_t>(self->scan_done_sem_));
            return 0;
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                self->conn_handle_ = event->connect.conn_handle;
            } else {
                self->conn_handle_ = 0xFFFF;
                self->set_error("connect failed");
            }
            xSemaphoreGive(static_cast<SemaphoreHandle_t>(self->connect_sem_));
            return 0;
        case BLE_GAP_EVENT_DISCONNECT:
            self->conn_handle_ = 0xFFFF;
            self->reset_handles();
            if (self->disconnect_callback_) {
                self->disconnect_callback_(event->disconnect.reason);
            }
            return 0;
        case BLE_GAP_EVENT_NOTIFY_RX: {
            RawNotification raw{};
            raw.attr_handle = event->notify_rx.attr_handle;
            const auto payload = mbuf_to_vector(event->notify_rx.om);
            raw.payload_len = std::min(payload.size(), sizeof(raw.payload));
            std::copy_n(payload.begin(), raw.payload_len, raw.payload);
            xQueueSend(static_cast<QueueHandle_t>(self->notify_queue_), &raw, 0);
            return 0;
        }
        default:
            return 0;
    }
}

int BleTransport::service_discovery_cb(
    std::uint16_t conn_handle,
    const struct ble_gatt_error *error,
    const struct ble_gatt_svc *service,
    void *arg) {
    auto *self = static_cast<BleTransport *>(arg);
    if (self == nullptr) {
        return 0;
    }

    if (error->status == 0 && service != nullptr) {
        self->service_found_ = true;
        self->pending_service_start_ = service->start_handle;
        self->pending_service_end_ = service->end_handle;
        return 0;
    }

    self->last_status_ = (error->status == BLE_HS_EDONE && self->service_found_) ? 0 : error->status;
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(self->gatt_sem_));
    return 0;
}

int BleTransport::characteristic_discovery_cb(
    std::uint16_t conn_handle,
    const struct ble_gatt_error *error,
    const struct ble_gatt_chr *chr,
    void *arg) {
    auto *self = static_cast<BleTransport *>(arg);
    if (self == nullptr) {
        return 0;
    }

    if (error->status == 0 && chr != nullptr) {
        const auto uuid16 = ble_uuid_u16(chr->uuid);
        switch (uuid16) {
            case 0xFF81: self->handles_.ff81 = chr->val_handle; break;
            case 0xFF82: self->handles_.ff82 = chr->val_handle; break;
            case 0xFF83: self->handles_.ff83 = chr->val_handle; break;
            case 0xFF84: self->handles_.ff84 = chr->val_handle; break;
            case 0xFF86: self->handles_.ff86 = chr->val_handle; break;
            case 0xFF87: self->handles_.ff87 = chr->val_handle; break;
            case 0xFF88: self->handles_.ff88 = chr->val_handle; break;
            case 0xFF89: self->handles_.ff89 = chr->val_handle; break;
            case 0xFF8A: self->handles_.ff8a = chr->val_handle; break;
            case 0xFF8B: self->handles_.ff8b = chr->val_handle; break;
            case 0xFF8C: self->handles_.ff8c = chr->val_handle; break;
            case 0xFF8D: self->handles_.ff8d = chr->val_handle; break;
            case 0xFF8E: self->handles_.ff8e = chr->val_handle; break;
            case 0xFF8F: self->handles_.ff8f = chr->val_handle; break;
            case 0x2A19: self->handles_.battery_level = chr->val_handle; break;
            default: break;
        }
        return 0;
    }

    self->last_status_ = (error->status == BLE_HS_EDONE) ? 0 : error->status;
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(self->gatt_sem_));
    return 0;
}

int BleTransport::read_cb(
    std::uint16_t conn_handle,
    const struct ble_gatt_error *error,
    struct ble_gatt_attr *attr,
    void *arg) {
    auto *self = static_cast<BleTransport *>(arg);
    if (self == nullptr) {
        return 0;
    }

    self->last_status_ = error->status;
    if (error->status == 0 && attr != nullptr) {
        self->last_read_payload_ = mbuf_to_vector(attr->om);
    } else {
        self->last_read_payload_.clear();
    }
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(self->gatt_sem_));
    return 0;
}

int BleTransport::write_cb(
    std::uint16_t conn_handle,
    const struct ble_gatt_error *error,
    struct ble_gatt_attr *attr,
    void *arg) {
    auto *self = static_cast<BleTransport *>(arg);
    if (self == nullptr) {
        return 0;
    }
    self->last_status_ = error->status;
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(self->gatt_sem_));
    return 0;
}

}  // namespace bridge

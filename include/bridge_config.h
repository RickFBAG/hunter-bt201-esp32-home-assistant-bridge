#pragma once

#if __has_include("bridge_secrets.h")
#include "bridge_secrets.h"
#else
#include "bridge_secrets.example.h"
#endif

namespace bridge::config {

inline constexpr const char *kWifiSsid = BRIDGE_WIFI_SSID;
inline constexpr const char *kWifiPassword = BRIDGE_WIFI_PASSWORD;
inline constexpr const char *kMqttUri = BRIDGE_MQTT_URI;
inline constexpr const char *kMqttUsername = BRIDGE_MQTT_USERNAME;
inline constexpr const char *kMqttPassword = BRIDGE_MQTT_PASSWORD;
inline constexpr const char *kDiscoveryPrefix = BRIDGE_DISCOVERY_PREFIX;
inline constexpr const char *kBaseTopic = BRIDGE_BASE_TOPIC;
inline constexpr const char *kDeviceId = BRIDGE_DEVICE_ID;
inline constexpr const char *kDeviceName = BRIDGE_DEVICE_NAME;
inline constexpr const char *kDeviceModel = BRIDGE_DEVICE_MODEL;
inline constexpr const char *kDeviceManufacturer = BRIDGE_DEVICE_MANUFACTURER;
inline constexpr const char *kHunterMac = BRIDGE_HUNTER_MAC;
#ifdef BRIDGE_HUNTER_NAME_HINT
inline constexpr const char *kHunterNameHint = BRIDGE_HUNTER_NAME_HINT;
#else
inline constexpr const char *kHunterNameHint = "Hunter BTT";
#endif
inline constexpr const char *kHunterPasscode = BRIDGE_HUNTER_PASSCODE;
inline constexpr unsigned long long kBatteryRefreshIntervalMs = BRIDGE_BATTERY_REFRESH_INTERVAL_MS;

}  // namespace bridge::config

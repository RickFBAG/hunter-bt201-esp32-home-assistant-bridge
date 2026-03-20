#pragma once

// Copy this file to include/bridge_secrets.h and fill in your values.

#define BRIDGE_WIFI_SSID "your-wifi-ssid"
#define BRIDGE_WIFI_PASSWORD "your-wifi-password"

#define BRIDGE_MQTT_URI "mqtt://192.168.1.10"
#define BRIDGE_MQTT_USERNAME "mqtt-username"
#define BRIDGE_MQTT_PASSWORD "mqtt-password"

#define BRIDGE_DISCOVERY_PREFIX "homeassistant"
#define BRIDGE_BASE_TOPIC "hunter_btt_bridge/hunter_bridge"
#define BRIDGE_DEVICE_ID "hunter_bridge"
#define BRIDGE_DEVICE_NAME "Hunter BTT201 Bridge"
#define BRIDGE_DEVICE_MODEL "Waveshare ESP32-C6 Touch AMOLED 1.8"
#define BRIDGE_DEVICE_MANUFACTURER "Waveshare"

#define BRIDGE_HUNTER_MAC "CC:03:7B:96:BC:AD"
#define BRIDGE_HUNTER_NAME_HINT "Hunter BTT"

// Evidence only proves FF81 read/write, not an unlock flow, so keep this empty in v1.
#define BRIDGE_HUNTER_PASSCODE ""

// Once every 24h by default. This is in milliseconds.
#define BRIDGE_BATTERY_REFRESH_INTERVAL_MS (24ULL * 60ULL * 60ULL * 1000ULL)

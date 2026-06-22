#pragma once

#include <Arduino.h>

// Keep private Wi-Fi credentials outside git. PlatformIO automatically adds
// the include directory, so a local include/creds.h is enough.
#if __has_include("creds.h")
#include "creds.h"
#endif

#ifndef D2
#define D2 4
#endif

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif

namespace AppConfig {

// Hardware layout for the current ESP8266 + WS2812B 8x8 build.
constexpr uint8_t kLedPin = D2;
constexpr uint8_t kMatrixWidth = 8;
constexpr uint8_t kMatrixHeight = 8;
constexpr uint16_t kLedCount = kMatrixWidth * kMatrixHeight;

// Full white on 64 WS2812B LEDs can pull too much current for small supplies,
// so the default is deliberately conservative.
constexpr uint8_t kDefaultBrightness = 20;

// Give USB power, the external LED supply, and the ESP8266 radio a moment to
// settle before LEDs and Wi-Fi start drawing burst current.
constexpr uint32_t kBootSettleDelayMs = 2000;

// Network behavior. Empty WIFI_SSID falls back to AP mode.
constexpr uint16_t kTcpPort = 7777;
constexpr char kAccessPointSsid[] = "led-matrix";
constexpr uint32_t kStationConnectTimeoutMs = 15000;
constexpr uint32_t kWifiRetryIntervalMs = 10000;
constexpr uint32_t kServerHealthCheckIntervalMs = 5000;
constexpr uint8_t kMaxCustomFrames = 8;
constexpr uint16_t kDefaultPresetIntervalMs = 140;
constexpr uint16_t kMinEffectFrameDelayMs = 20;

}  // namespace AppConfig

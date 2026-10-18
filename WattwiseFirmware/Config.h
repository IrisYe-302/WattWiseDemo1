#pragma once

#include <Arduino.h>

// Select the board before each upload:
//   Smart master:        WATTWISE_HOME_ID 0, WATTWISE_IS_MASTER 1
//   Conventional slave: WATTWISE_HOME_ID 1, WATTWISE_IS_MASTER 0
// Upload node 0 first, then change these two values and upload node 1.
#ifndef WATTWISE_HOME_ID
#define WATTWISE_HOME_ID 1
#endif

#ifndef WATTWISE_IS_MASTER
#define WATTWISE_IS_MASTER 0
#endif

#if WATTWISE_HOME_ID < 0 || WATTWISE_HOME_ID > 1
#error "WATTWISE_HOME_ID must be 0 or 1"
#endif

#if WATTWISE_IS_MASTER && WATTWISE_HOME_ID != 0
#error "The master is node 0"
#endif

namespace wattwise::config {

constexpr uint8_t kWifiChannel = 1;
constexpr char kApSsid[] = "Wattwise";
constexpr char kApPassword[] = "wattwise-demo";  // WPA2 requires 8+ characters.
constexpr uint8_t kMaxApClients = 4;

// Active-high LEDs, each with its own current-limiting resistor.
constexpr uint8_t kLoadLedPins[3] = {6, 3, 14};
constexpr uint8_t kGridGoodLedPin = 4;  // Master only.
constexpr uint8_t kGridOutLedPin = 5;   // Master only.

// Accelerated simulated battery runtimes for a short physical demo.
constexpr uint32_t kConventionalOutageRuntimeMs = 20000;
constexpr uint32_t kSmartOutageRuntimeMs = 45000;

constexpr uint32_t kCommandPeriodMs = 200;
constexpr uint32_t kTelemetryPeriodMs = 500;
constexpr uint32_t kNodeOfflineMs = 2500;
constexpr uint32_t kCommandWatchdogMs = 5000;
constexpr uint32_t kSlowBlinkMs = 900;
constexpr uint32_t kFastBlinkMs = 180;

}  // namespace wattwise::config

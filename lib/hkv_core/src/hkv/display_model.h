#pragma once
// What is shown on the local display, independent of the display technology.
// ST7735 TFT today; on the CC1312 probably a segment LCD / e-paper with fewer pages.

#include <math.h>
#include <stdint.h>

#include "hkv/allocator.h"
#include "hkv/calendar.h"

namespace hkv {

enum class Page : uint8_t {
  Consumption = 0,  // overview: temperatures, dT, current and previous period
  Temperatures,     // radiator, room, dT (large, live)
  Network,          // connection / setup / connecting / failed (depends on NetInfo::mode)
  Count,            // number of pages in the button cycle
  BatteryLow,       // warning, shown before the cycle when the battery is low
  Calibration,      // calibration run: progress and sensor values (shown first while it runs)
};

// Error flags (also used for the OMS status byte).
enum ErrorFlag : uint32_t {
  kErrRadiatorSensor = 1u << 0,
  kErrRoomSensor = 1u << 1,
  kErrBatteryLow = 1u << 2,
  kErrTimeNotSet = 1u << 3,
  kErrStorage = 1u << 4,
};

// Communication status, filled by the port (WiFi on ESP32, radio on CC1312).
struct NetInfo {
  enum class Mode : uint8_t { None, Off, Connecting, Connected, AccessPoint, Failed };
  enum class Mqtt : uint8_t { Disabled, Disconnected, Connected };

  Mode mode = Mode::None;
  char ssid[33] = {0};
  char ip[16] = {0};
  uint8_t channel = 0;
  int8_t rssi = 0;

  uint8_t attempt = 0;        // Connecting: current attempt (1-based)
  uint8_t maxAttempts = 0;
  uint8_t progressPct = 0;    // Connecting: overall progress, Failed: countdown progress
  uint8_t secondsLeft = 0;    // Connecting: left in this attempt, Failed: until setup mode

  char apSsid[33] = {0};
  char apPass[33] = {0};
  char apIp[16] = {0};

  Mqtt mqtt = Mqtt::Disabled;
  uint32_t mqttAgeS = UINT32_MAX;  // seconds since the last successful publish

  char mac[18] = {0};
  char firmware[12] = {0};
};

struct DisplayModel {
  Page page = Page::Consumption;
  uint32_t uptimeMs = 0;
  uint32_t meterId = 0;

  uint64_t currentMilliUnits = 0;
  bool periodStartValid = false;
  DateTime periodStart;           // first day of the current billing period
  bool dueDateValid = false;
  DateTime dueDate;               // end of the previous billing period
  uint64_t dueMilliUnits = 0;

  AllocResult alloc;
  bool live = false;              // temperatures are live readings (display only)
  bool radiatorValid = false;
  bool roomValid = false;
  float humidityPct = NAN;
  float pressureHpa = NAN;

  bool batteryValid = false;
  uint16_t batteryMv = 0;
  uint8_t batteryPercent = 0;
  bool charging = false;
  bool batteryLow = false;

  bool timeValid = false;
  DateTime local;
  uint32_t errors = 0;

  NetInfo net;

  // Calibration run (page Calibration). Temperatures: raw values, NAN = not available.
  struct Calib {
    uint8_t state = 0;          // CalibState: 0 idle, 1 running, 2 done, 3 ended early
    uint32_t elapsedS = 0;
    uint32_t durationS = 0;
    uint32_t warmupS = 0;
    float radiatorC = NAN;      // incl. radiator offset
    float roomRawC = NAN;       // built-in room sensor without offset
    float referenceC = NAN;
    char refName[8] = {0};
  } calib;
};

}  // namespace hkv

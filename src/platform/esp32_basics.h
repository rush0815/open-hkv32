#pragma once
// Small ESP32 implementations of core HAL interfaces.

#include <Arduino.h>
#include <Preferences.h>
#include <sys/time.h>
#include <time.h>

#include "hkv/hal.h"

namespace port {

class Esp32Clock : public hkv::IClock {
 public:
  uint32_t nowUtc() override { return static_cast<uint32_t>(time(nullptr)); }
  // Anything before 2024 means "never synchronised" (ESP32 starts at 1970 after power-up).
  bool isTimeValid() override { return time(nullptr) > 1704067200; }
  void setUtc(uint32_t utc) override {
    timeval tv{static_cast<time_t>(utc), 0};
    settimeofday(&tv, nullptr);
  }
  uint32_t uptimeMs() override { return millis(); }
};

class Esp32Storage : public hkv::IStorage {
 public:
  bool begin() { return prefs_.begin("hkv", false); }
  bool read(const char* key, void* buf, size_t len) override {
    if (!prefs_.isKey(key)) return false;  // avoids an error log for keys never written
    if (prefs_.getBytesLength(key) != len) return false;
    return prefs_.getBytes(key, buf, len) == len;
  }
  bool write(const char* key, const void* buf, size_t len) override {
    return prefs_.putBytes(key, buf, len) == len;
  }
  bool erase(const char* key) override { return prefs_.remove(key); }

 private:
  Preferences prefs_;
};

class Esp32Random : public hkv::IRandom {
 public:
  uint32_t next() override { return esp_random(); }
};

}  // namespace port

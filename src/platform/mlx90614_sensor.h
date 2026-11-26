#pragma once
// MLX90614 infrared thermometer (object temperature = radiator surface).
// Minimal SMBus driver with PEC check, no external library -> easy to port to the
// CC1312 I2C driver. Only READS the sensor; the EEPROM is never written here.
// Tested on a GY-906 module (MLX90614ESF-BAA, 3 V variant) at 3V3, address 0x5A.
//
// Hints: use the 3 V variant (MLX90614xBx); xAx is the 5 V variant. SMBus max 100 kHz.
// Painted radiators have an emissivity of ~0.93..0.95; the factory setting is 1.0.

#include <Wire.h>

#include "hkv/hal.h"

namespace port {

class Mlx90614Sensor : public hkv::ITemperatureSensor {
 public:
  explicit Mlx90614Sensor(TwoWire& wire, uint8_t address = 0x5A) : wire_(wire), addr_(address) {}

  bool begin() override {
    float t;
    return readTemp(kRamTambient, t);
  }

  hkv::TempReading read() override {
    hkv::TempReading r;
    float t;
    if (!readTemp(kRamTobject1, t)) return r;
    if (emissivity_ < 0.9999f) {
      float ta;
      if (!readTemp(kRamTambient, ta)) return r;
      t = hkv::correctEmissivity(t, ta, emissivity_);  // sensor EEPROM stays at its factory 1.0
    }
    if (t > -40.0f && t < 125.0f) {
      r.valid = true;
      r.celsius = t;
    }
    return r;
  }

  void setEmissivity(float e) override { emissivity_ = e; }

  const char* name() const override { return "MLX90614"; }

  // --- diagnostics (read only) ---
  bool ambient(float& t) { return readTemp(kRamTambient, t); }       // sensor housing
  bool object1(float& t) { return readTemp(kRamTobject1, t); }
  bool object2(float& t) {  // only dual-zone variants; single-zone parts return 0
    uint16_t raw;
    if (!readWord(kRamTobject2, raw) || raw == 0 || (raw & 0x8000)) return false;
    t = raw * 0.02f - 273.15f;
    return true;
  }
  bool emissivity(float& e) {
    uint16_t raw;
    if (!readWord(kEepEmissivity, raw)) return false;
    e = raw / 65535.0f;
    return true;
  }
  bool config1(uint16_t& v) { return readWord(kEepConfig1, v); }
  bool smbusAddress(uint8_t& a) {
    uint16_t v;
    if (!readWord(kEepAddress, v)) return false;
    a = static_cast<uint8_t>(v & 0x7F);
    return true;
  }
  bool id(uint16_t out[4]) {
    for (int i = 0; i < 4; ++i)
      if (!readWord(static_cast<uint8_t>(kEepId0 + i), out[i])) return false;
    return true;
  }
  uint8_t address() const { return addr_; }

 private:
  // RAM (0x00..0x1F) and EEPROM (0x20 | address) commands
  static constexpr uint8_t kRamTambient = 0x06;
  static constexpr uint8_t kRamTobject1 = 0x07;
  static constexpr uint8_t kRamTobject2 = 0x08;
  static constexpr uint8_t kEepEmissivity = 0x24;
  static constexpr uint8_t kEepConfig1 = 0x25;
  static constexpr uint8_t kEepAddress = 0x2E;
  static constexpr uint8_t kEepId0 = 0x3C;

  static uint8_t crc8(uint8_t crc, uint8_t data) {
    crc ^= data;
    for (int i = 0; i < 8; ++i) crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x07) : static_cast<uint8_t>(crc << 1);
    return crc;
  }

  // SMBus "read word" with PEC (CRC-8) check.
  bool readWord(uint8_t cmd, uint16_t& value) {
    wire_.beginTransmission(addr_);
    wire_.write(cmd);
    if (wire_.endTransmission(false) != 0) return false;  // repeated start
    if (wire_.requestFrom(static_cast<int>(addr_), 3) != 3) return false;
    const uint8_t lo = wire_.read();
    const uint8_t hi = wire_.read();
    const uint8_t pec = wire_.read();
    uint8_t c = 0;
    c = crc8(c, static_cast<uint8_t>(addr_ << 1));
    c = crc8(c, cmd);
    c = crc8(c, static_cast<uint8_t>((addr_ << 1) | 1));
    c = crc8(c, lo);
    c = crc8(c, hi);
    if (c != pec) return false;
    value = static_cast<uint16_t>((hi << 8) | lo);
    return true;
  }

  bool readTemp(uint8_t cmd, float& celsius) {
    uint16_t raw;
    if (!readWord(cmd, raw)) return false;
    if (raw & 0x8000) return false;  // error flag
    celsius = raw * 0.02f - 273.15f;
    return true;
  }

  TwoWire& wire_;
  uint8_t addr_;
  float emissivity_ = 1.0f;
};

}  // namespace port

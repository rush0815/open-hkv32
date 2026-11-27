#pragma once
// Temporary reference room sensor for the calibration run (Service -> Kalibrierung Fühler).
// It hangs on the same I2C bus as the other sensors and is only plugged in for calibration.
// Detected automatically, also when plugged in later:
//   - Sensirion SHT4x / STS4x at 0x44 / 0x45 / 0x46 (recommended, +-0.2 C)
//   - a second BMP280 on the address the built-in one does not use (SDO -> 3V3 = 0x77),
//     only +-1 K absolute
// Hang it in free air, at least 1 m away from the radiator, not in the sun.

#include <Adafruit_BMP280.h>
#include <Arduino.h>
#include <Wire.h>

#include "hkv/hal.h"
#include "platform/bmp280_room_sensor.h"

namespace port {

class ReferenceSensor : public hkv::ITemperatureSensor {
 public:
  ReferenceSensor(TwoWire& wire, const Bmp280RoomSensor& builtIn) : wire_(wire), builtIn_(builtIn), bmp_(&wire) {}

  bool begin() override { return probe(); }

  hkv::TempReading read() override {
    hkv::TempReading r;
    if (kind_ == Kind::None && !probe()) return r;
    if (kind_ == Kind::Sht4x) {
      float t, rh;
      if (!sht4xMeasure(shtAddr_, t, rh)) {
        kind_ = Kind::None;  // unplugged? probe again next time
        return r;
      }
      r.valid = t > -40.0f && t < 85.0f;
      r.celsius = t;
      r.humidityPct = rh < 0.0f ? 0.0f : rh > 100.0f ? 100.0f : rh;
      return r;
    }
    if (!bmp_.takeForcedMeasurement()) {
      kind_ = Kind::None;
      return r;
    }
    const float t = bmp_.readTemperature();
    r.valid = !isnan(t) && t > -40.0f && t < 85.0f;
    r.celsius = t;
    if (!r.valid) kind_ = Kind::None;
    return r;
  }

  const char* name() const override {
    switch (kind_) {
      case Kind::Sht4x: return "SHT4x";
      case Kind::Bmp280: return "BMP280";
      case Kind::None: break;
    }
    return "-";
  }

 private:
  enum class Kind : uint8_t { None, Sht4x, Bmp280 };

  bool probe() {
    kind_ = Kind::None;
    for (uint8_t a : {0x44, 0x45, 0x46}) {
      float t, rh;
      if (sht4xMeasure(a, t, rh)) {
        shtAddr_ = a;
        kind_ = Kind::Sht4x;
        return true;
      }
    }
    const uint8_t own = builtIn_.address();
    const uint8_t other = own == 0x77 ? 0x76 : 0x77;
    if (own != 0 && bmp_.begin(other)) {
      bmp_.setSampling(Adafruit_BMP280::MODE_FORCED, Adafruit_BMP280::SAMPLING_X2, Adafruit_BMP280::SAMPLING_X1,
                       Adafruit_BMP280::FILTER_OFF, Adafruit_BMP280::STANDBY_MS_1);
      kind_ = Kind::Bmp280;
      return true;
    }
    return false;
  }

  // High-precision single shot (command 0xFD, max. 8.3 ms), both words CRC checked.
  bool sht4xMeasure(uint8_t addr, float& t, float& rh) {
    wire_.beginTransmission(addr);
    wire_.write(0xFD);
    if (wire_.endTransmission() != 0) return false;
    delay(10);
    if (wire_.requestFrom(addr, static_cast<uint8_t>(6)) != 6) return false;
    uint8_t b[6];
    for (uint8_t& v : b) v = static_cast<uint8_t>(wire_.read());
    if (crc8(b) != b[2] || crc8(b + 3) != b[5]) return false;
    const uint16_t rt = static_cast<uint16_t>(b[0] << 8 | b[1]);
    const uint16_t rr = static_cast<uint16_t>(b[3] << 8 | b[4]);
    t = -45.0f + 175.0f * rt / 65535.0f;
    rh = -6.0f + 125.0f * rr / 65535.0f;
    return true;
  }

  static uint8_t crc8(const uint8_t* d) {  // Sensirion: poly 0x31, init 0xFF
    uint8_t crc = 0xFF;
    for (int i = 0; i < 2; ++i) {
      crc ^= d[i];
      for (int k = 0; k < 8; ++k) crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x31) : static_cast<uint8_t>(crc << 1);
    }
    return crc;
  }

  TwoWire& wire_;
  const Bmp280RoomSensor& builtIn_;
  Adafruit_BMP280 bmp_;
  Kind kind_ = Kind::None;
  uint8_t shtAddr_ = 0x44;
};

}  // namespace port

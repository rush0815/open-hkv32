#pragma once
// Room (air) temperature + air pressure from a BMP280 in forced mode (sensor sleeps between readings).
// The BMP280 has no humidity sensor; a BME280 would additionally deliver it.
// Note: the BMP280 measures its own die temperature - keep it away from the MCU,
// display and radiator plate, and calibrate with Settings::roomOffsetC.

#include <Adafruit_BMP280.h>
#include <Wire.h>

#include "hkv/hal.h"

namespace port {

class Bmp280RoomSensor : public hkv::ITemperatureSensor {
 public:
  explicit Bmp280RoomSensor(TwoWire& wire) : bmp_(&wire) {}

  bool begin() override {
    ready_ = bmp_.begin(0x76) || bmp_.begin(0x77);
    if (ready_) {
      bmp_.setSampling(Adafruit_BMP280::MODE_FORCED, Adafruit_BMP280::SAMPLING_X2,
                       Adafruit_BMP280::SAMPLING_X4, Adafruit_BMP280::FILTER_OFF,
                       Adafruit_BMP280::STANDBY_MS_1);
    }
    return ready_;
  }

  hkv::TempReading read() override {
    hkv::TempReading r;
    if (!ready_ && !begin()) return r;  // retry, e.g. after a loose wire
    if (!bmp_.takeForcedMeasurement()) {
      ready_ = false;
      return r;
    }
    const float t = bmp_.readTemperature();
    r.valid = !isnan(t) && t > -40.0f && t < 85.0f;
    r.celsius = t;
    const float pa = bmp_.readPressure();
    if (!isnan(pa) && pa > 30000.0f && pa < 110000.0f) r.pressureHpa = pa / 100.0f;
    return r;
  }

  const char* name() const override { return "BMP280"; }

 private:
  Adafruit_BMP280 bmp_;
  bool ready_ = false;
};

}  // namespace port

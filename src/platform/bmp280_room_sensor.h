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
    // Once found, stay on that address: a reference BMP280 for the calibration run may
    // sit on the other one (see reference_sensor.h).
    if (addr_) {
      ready_ = bmp_.begin(addr_);
    } else if (bmp_.begin(0x76)) {
      addr_ = 0x76;
      ready_ = true;
    } else if (bmp_.begin(0x77)) {
      addr_ = 0x77;
      ready_ = true;
    }
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
  uint8_t address() const { return addr_; }  // 0 = not found yet

 private:
  Adafruit_BMP280 bmp_;
  uint8_t addr_ = 0;
  bool ready_ = false;
};

}  // namespace port

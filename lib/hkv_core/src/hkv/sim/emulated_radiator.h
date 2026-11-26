#pragma once
// Emulated radiator surface sensor (stand-in for the MLX90614 until it is available).
//
// Model: the radiator alternates between "valve open" and "valve closed" phases of
// random length. While open it heats towards a random supply temperature, while
// closed it cools towards the room temperature (first order, time constant tau).
// Gaussian-ish sensor noise is added. Pure core code -> usable on every port and in tests.

#include "hkv/hal.h"

namespace hkv {

struct EmulatedRadiatorConfig {
  float roomC = 20.0f;
  float supplyMinC = 40.0f;
  float supplyMaxC = 62.0f;
  float tauS = 900.0f;          // 15 min thermal time constant
  uint32_t phaseMinS = 600;     // 10 min
  uint32_t phaseMaxS = 5400;    // 90 min
  float onProbability = 0.55f;
  float noiseC = 0.15f;
  float timeScale = 1.0f;       // >1 = faster simulation for tests on the desk
};

class EmulatedRadiatorSensor : public ITemperatureSensor {
 public:
  EmulatedRadiatorSensor(IClock& clock, IRandom& rnd, const EmulatedRadiatorConfig& cfg = EmulatedRadiatorConfig())
      : clock_(clock), rnd_(rnd), cfg_(cfg) {}

  bool begin() override;
  TempReading read() override;
  const char* name() const override { return "MLX90614 (emulated)"; }

  bool valveOpen() const { return open_; }

 private:
  void newPhase();

  IClock& clock_;
  IRandom& rnd_;
  EmulatedRadiatorConfig cfg_;
  float tempC_ = 20.0f;
  float targetC_ = 20.0f;
  bool open_ = false;
  float phaseLeftS_ = 0.0f;
  uint32_t lastMs_ = 0;
};

// Small deterministic PRNG (xorshift32) for the core / tests.
class XorShiftRandom : public IRandom {
 public:
  explicit XorShiftRandom(uint32_t seed) : s_(seed ? seed : 0x12345678u) {}
  uint32_t next() override {
    s_ ^= s_ << 13;
    s_ ^= s_ >> 17;
    s_ ^= s_ << 5;
    return s_;
  }

 private:
  uint32_t s_;
};

}  // namespace hkv

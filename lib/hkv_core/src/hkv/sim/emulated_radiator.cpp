#include "hkv/sim/emulated_radiator.h"

#include <math.h>

namespace hkv {

bool EmulatedRadiatorSensor::begin() {
  tempC_ = cfg_.roomC;
  lastMs_ = clock_.uptimeMs();
  newPhase();
  return true;
}

void EmulatedRadiatorSensor::newPhase() {
  open_ = rnd_.uniform() < cfg_.onProbability;
  targetC_ = open_ ? cfg_.supplyMinC + rnd_.uniform() * (cfg_.supplyMaxC - cfg_.supplyMinC) : cfg_.roomC;
  phaseLeftS_ = static_cast<float>(cfg_.phaseMinS) +
                rnd_.uniform() * static_cast<float>(cfg_.phaseMaxS - cfg_.phaseMinS);
}

TempReading EmulatedRadiatorSensor::read() {
  const uint32_t now = clock_.uptimeMs();
  float dt = static_cast<float>(now - lastMs_) / 1000.0f * cfg_.timeScale;
  lastMs_ = now;

  // Integrate in steps so that phase changes inside a long interval are respected.
  while (dt > 0.0f) {
    const float step = dt < phaseLeftS_ ? dt : phaseLeftS_;
    tempC_ = targetC_ + (tempC_ - targetC_) * expf(-step / cfg_.tauS);
    dt -= step;
    phaseLeftS_ -= step;
    if (phaseLeftS_ <= 0.0f) newPhase();
  }

  // Sum of uniforms approximates a normal distribution.
  const float noise = (rnd_.uniform() + rnd_.uniform() + rnd_.uniform() - 1.5f) * 2.0f * cfg_.noiseC;
  TempReading r;
  r.valid = true;
  r.celsius = tempC_ + noise;
  return r;
}

}  // namespace hkv

#include "hkv/allocator.h"

#include <math.h>

namespace hkv {

float correctEmissivity(float tMeasC, float tAmbC, float emissivity) {
  if (!(emissivity > 0.0f) || emissivity >= 0.9999f) return tMeasC;
  const float tm = tMeasC + 273.15f, ta = tAmbC + 273.15f;
  const float tm4 = tm * tm * tm * tm, ta4 = ta * ta * ta * ta;
  const float to4 = ta4 + (tm4 - ta4) / emissivity;
  if (to4 <= 0.0f) return tMeasC;
  return sqrtf(sqrtf(to4)) - 273.15f;
}

float correctRoomCoupling(float measuredRoomC, float radiatorC, float coupling) {
  if (!(coupling > 0.0f) || coupling >= 0.95f) return measuredRoomC;
  return (measuredRoomC - coupling * radiatorC) / (1.0f - coupling);
}

float couplingFromReference(float measuredRoomC, float radiatorC, float referenceRoomC) {
  const float span = radiatorC - referenceRoomC;
  if (span < 5.0f) return -1.0f;
  float c = (measuredRoomC - referenceRoomC) / span;
  if (c < 0.0f) c = 0.0f;
  return c;
}

const char* countStateName(CountState s) {
  switch (s) {
    case CountState::Idle: return "idle";
    case CountState::Counting: return "counting";
    case CountState::Summer: return "summer";
    case CountState::SensorError: return "sensor_error";
  }
  return "?";
}

AllocResult evaluate(const Settings& s, const TempReading& radiator, const TempReading& room, bool summer,
                     float couplingRadiatorC) {
  AllocResult r;
  if (!radiator.valid || isnan(radiator.celsius)) {
    r.state = CountState::SensorError;
    return r;
  }
  r.radiatorC = radiator.celsius + s.radiatorOffsetC;
  if (room.valid && !isnan(room.celsius)) {
    const float radForCoupling = isnan(couplingRadiatorC) ? r.radiatorC : couplingRadiatorC + s.radiatorOffsetC;
    r.roomC = correctRoomCoupling(room.celsius + s.roomOffsetC, radForCoupling, s.roomCoupling);
  } else {
    r.roomC = kFallbackRoomC;
    r.roomFallback = true;
  }
  r.deltaK = r.radiatorC - r.roomC;

  if (r.deltaK < s.startDeltaK || r.radiatorC < s.minRadiatorC) {
    r.state = CountState::Idle;
    return r;
  }
  if (summer && r.radiatorC < s.summerMinRadiatorC) {
    r.state = CountState::Summer;
    return r;
  }

  const float k = s.kc * (s.scale == ScaleMode::Product ? s.kq : 1.0f);
  r.unitsPerHour = k * powf(r.deltaK / s.nominalDeltaK, s.exponent);
  r.state = CountState::Counting;
  return r;
}

}  // namespace hkv

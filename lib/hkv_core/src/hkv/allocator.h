#pragma once
// Heat cost allocation algorithm (two-sensor principle, EN 834 style).
//
//   dT    = (T_radiator + offset) - (T_room + offset)
//   rate  = K * (dT / dT_nominal)^n          [units per hour]
//   K     = Kc            (unit scale)
//         = Kc * Kq[kW]   (product scale)
//
// Counting only happens while dT >= startDeltaK and T_radiator >= minRadiatorC
// (inside the summer period the stricter summerMinRadiatorC applies).
// If the room sensor fails the device falls back to a fixed reference room
// temperature, like single-sensor allocators do.

#include "hkv/hal.h"
#include "hkv/settings.h"

namespace hkv {

enum class CountState : uint8_t {
  Idle = 0,         // below start threshold
  Counting = 1,
  Summer = 2,       // suppressed by summer threshold
  SensorError = 3,  // radiator temperature unavailable
  Uncalibrated = 4, // no calibration run yet - the meter does not count
  Calibrating = 5,  // calibration run in progress - no counting
};

const char* countStateName(CountState s);

struct AllocResult {
  CountState state = CountState::Idle;
  float radiatorC = 0.0f;   // corrected
  float roomC = 0.0f;       // corrected (or fallback)
  float deltaK = 0.0f;
  float unitsPerHour = 0.0f;
  bool roomFallback = false;
};

constexpr float kFallbackRoomC = 20.0f;

// couplingRadiatorC: radiator temperature (raw, without offset) for the room-sensor coupling
// correction, normally the time-lagged value (see Settings::roomCouplingTauMin). NAN = use
// the current radiator reading.
AllocResult evaluate(const Settings& s, const TempReading& radiator, const TempReading& room, bool summer,
                     float couplingRadiatorC = NAN);

// Removes the radiator's influence on the room sensor:
// T_room = (T_measured - c * T_radiator) / (1 - c). c = 0 returns T_measured unchanged.
float correctRoomCoupling(float measuredRoomC, float radiatorC, float coupling);

// Calibration: coupling c from one reference room temperature (e.g. a thermometer).
// Returns a negative value if the radiator is not warm enough (needs >= 5 K above the room).
float couplingFromReference(float measuredRoomC, float radiatorC, float referenceRoomC);

}  // namespace hkv

#include "hkv/settings.h"

#include <string.h>

#include "hkv/calendar.h"

namespace hkv {

void setDefaults(Settings& s, uint32_t defaultMeterId) {
  memset(&s, 0, sizeof(s));
  s.meterId = defaultMeterId % 100000000u;
  // "XXX" is NOT a registered manufacturer id. Real deployments need an own FLAG id.
  memcpy(s.manufacturer, "XXX", 4);
  s.deviceVersion = 0x01;

  s.scale = ScaleMode::Unit;
  s.kc = 1.0f;
  s.kq = 1.0f;
  s.exponent = 1.3f;
  s.nominalDeltaK = 50.0f;
  s.startDeltaK = 4.5f;
  s.minRadiatorC = 25.0f;
  s.summerMinRadiatorC = 40.0f;
  s.summerStartMonth = 6;
  s.summerEndMonth = 8;
  s.radiatorOffsetC = 0.0f;
  s.roomOffsetC = 0.0f;

  s.dueMonth = 12;
  s.dueDay = 31;

  s.measureIntervalS = 120;
  s.telegramIntervalS = 240;
  s.utcOffsetMin = 60;  // CET
  s.euDst = 1;

  s.batteryLowMv = 3400;

  s.omsSecurity = static_cast<uint8_t>(OmsSecurity::None);
  // key stays all-zero until configured

  s.displayTimeoutS = 15;
  s.emissivity = 1.0f;
  s.displayInvert = 1;
  s.displayFlip = 0;
  s.roomCoupling = 0.0f;
  s.roomCouplingTauMin = 10.0f;
  s.calibrated = 0;
}

static bool inRange(float v, float lo, float hi) { return v >= lo && v <= hi; }

const char* validate(const Settings& s) {
  if (s.meterId > 99999999u) return "meterId must have at most 8 digits";
  for (int i = 0; i < 3; ++i) {
    if (s.manufacturer[i] < 'A' || s.manufacturer[i] > 'Z') return "manufacturer must be 3 letters A-Z";
  }
  if (s.manufacturer[3] != '\0') return "manufacturer must be 3 letters A-Z";
  if (s.scale != ScaleMode::Unit && s.scale != ScaleMode::Product) return "invalid scale";
  if (!inRange(s.kc, 0.05f, 20.0f)) return "Kc out of range (0.05..20)";
  if (!inRange(s.kq, 0.01f, 20.0f)) return "Kq out of range (0.01..20 kW)";
  if (!inRange(s.exponent, 1.0f, 2.0f)) return "exponent out of range (1.0..2.0)";
  if (!inRange(s.nominalDeltaK, 10.0f, 80.0f)) return "nominal dT out of range (10..80 K)";
  if (!inRange(s.startDeltaK, 1.0f, 20.0f)) return "start dT out of range (1..20 K)";
  if (!inRange(s.minRadiatorC, 15.0f, 60.0f)) return "min radiator temp out of range (15..60 C)";
  if (!inRange(s.summerMinRadiatorC, 15.0f, 90.0f)) return "summer min radiator temp out of range";
  if (s.summerStartMonth > 12 || s.summerEndMonth > 12) return "summer months must be 0..12";
  if ((s.summerStartMonth == 0) != (s.summerEndMonth == 0)) return "set both summer months or none";
  if (!inRange(s.radiatorOffsetC, -10.0f, 10.0f) || !inRange(s.roomOffsetC, -10.0f, 10.0f))
    return "offsets must be within +-10 K";
  if (s.dueMonth < 1 || s.dueMonth > 12) return "due month must be 1..12";
  if (s.dueDay < 1 || s.dueDay > daysInMonth(2001, s.dueMonth)) return "invalid due day (29.02 not allowed)";
  if (s.measureIntervalS < 10 || s.measureIntervalS > 3600) return "measure interval 10..3600 s";
  if (s.telegramIntervalS < 10 || s.telegramIntervalS > 43200) return "telegram interval 10..43200 s";
  if (s.utcOffsetMin < -720 || s.utcOffsetMin > 840) return "invalid UTC offset";
  if (s.euDst > 1) return "euDst must be 0 or 1";
  if (s.batteryLowMv < 1800 || s.batteryLowMv > 4200) return "battery low threshold 1800..4200 mV";
  if (s.omsSecurity != static_cast<uint8_t>(OmsSecurity::None) &&
      s.omsSecurity != static_cast<uint8_t>(OmsSecurity::Mode5))
    return "OMS security must be 0 or 5";
  if (s.displayTimeoutS < 3 || s.displayTimeoutS > 600) return "display timeout 3..600 s";
  if (!inRange(s.emissivity, 0.10f, 1.0f)) return "emissivity must be 0.10..1.00";
  if (s.displayInvert > 1 || s.displayFlip > 1) return "display options must be 0 or 1";
  if (!inRange(s.roomCoupling, 0.0f, 0.8f)) return "room sensor coupling must be 0..0.8";
  if (!inRange(s.roomCouplingTauMin, 0.0f, 60.0f)) return "coupling time constant must be 0..60 min";
  if (s.calibrated > 1) return "calibrated must be 0 or 1";
  return nullptr;
}

bool isSummer(const Settings& s, uint8_t month) {
  if (s.summerStartMonth == 0) return false;
  if (s.summerStartMonth <= s.summerEndMonth) {
    return month >= s.summerStartMonth && month <= s.summerEndMonth;
  }
  return month >= s.summerStartMonth || month <= s.summerEndMonth;  // wraps over new year
}

}  // namespace hkv

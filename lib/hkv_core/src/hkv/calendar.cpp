#include "hkv/calendar.h"

namespace hkv {

// Algorithms from Howard Hinnant, "chrono-Compatible Low-Level Date Algorithms".
int32_t daysFromCivil(int32_t y, uint32_t m, uint32_t d) {
  y -= m <= 2 ? 1 : 0;
  const int32_t era = (y >= 0 ? y : y - 399) / 400;
  const uint32_t yoe = static_cast<uint32_t>(y - era * 400);
  const uint32_t mp = m > 2 ? m - 3 : m + 9;
  const uint32_t doy = (153 * mp + 2) / 5 + d - 1;
  const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int32_t>(doe) - 719468;
}

static void civilFromDays(int32_t z, int32_t& y, uint32_t& m, uint32_t& d) {
  z += 719468;
  const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  const uint32_t doe = static_cast<uint32_t>(z - era * 146097);
  const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = static_cast<int32_t>(yoe) + era * 400;
  const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const uint32_t mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y += m <= 2 ? 1 : 0;
}

uint8_t daysInMonth(uint16_t year, uint8_t month) {
  static const uint8_t kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  if (month == 2) {
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return leap ? 29 : 28;
  }
  return kDays[month - 1];
}

DateTime fromUnix(uint32_t utc) {
  const int32_t days = static_cast<int32_t>(utc / 86400u);
  const uint32_t rem = utc % 86400u;
  int32_t y;
  uint32_t m, d;
  civilFromDays(days, y, m, d);
  DateTime dt;
  dt.year = static_cast<uint16_t>(y);
  dt.month = static_cast<uint8_t>(m);
  dt.day = static_cast<uint8_t>(d);
  dt.hour = static_cast<uint8_t>(rem / 3600);
  dt.minute = static_cast<uint8_t>((rem / 60) % 60);
  dt.second = static_cast<uint8_t>(rem % 60);
  dt.weekday = static_cast<uint8_t>((days + 4) % 7);  // 1970-01-01 was a Thursday
  return dt;
}

uint32_t toUnix(const DateTime& dt) {
  const int32_t days = daysFromCivil(dt.year, dt.month, dt.day);
  return static_cast<uint32_t>(days) * 86400u + dt.hour * 3600u + dt.minute * 60u + dt.second;
}

static uint32_t lastSundayUtc(uint16_t year, uint8_t month, uint8_t hourUtc) {
  DateTime dt;
  dt.year = year;
  dt.month = month;
  dt.day = daysInMonth(year, month);
  const int32_t days = daysFromCivil(dt.year, dt.month, dt.day);
  const int32_t weekday = (days + 4) % 7;  // 0 = Sunday
  dt.day = static_cast<uint8_t>(dt.day - weekday);
  dt.hour = hourUtc;
  return toUnix(dt);
}

bool isEuSummerTime(uint32_t utc) {
  const uint16_t year = fromUnix(utc).year;
  return utc >= lastSundayUtc(year, 3, 1) && utc < lastSundayUtc(year, 10, 1);
}

DateTime toLocal(uint32_t utc, int16_t utcOffsetMinutes, bool euDst) {
  const bool dst = euDst && isEuSummerTime(utc);
  const int32_t shift = utcOffsetMinutes * 60 + (dst ? 3600 : 0);
  DateTime dt = fromUnix(static_cast<uint32_t>(static_cast<int64_t>(utc) + shift));
  dt.dst = dst;
  return dt;
}

}  // namespace hkv

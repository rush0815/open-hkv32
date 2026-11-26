#pragma once
// Calendar helpers without libc time functions (newlib/TI RTS differ in time zone support).

#include <stdint.h>

namespace hkv {

struct DateTime {
  uint16_t year = 1970;
  uint8_t month = 1;    // 1..12
  uint8_t day = 1;      // 1..31
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;
  uint8_t weekday = 4;  // 0 = Sunday
  bool dst = false;     // true if summer time was applied (local time only)
};

int32_t daysFromCivil(int32_t year, uint32_t month, uint32_t day);
uint8_t daysInMonth(uint16_t year, uint8_t month);
DateTime fromUnix(uint32_t utc);
uint32_t toUnix(const DateTime& dt);  // interprets dt as UTC

// EU rule: summer time from last Sunday of March 01:00 UTC to last Sunday of October 01:00 UTC.
bool isEuSummerTime(uint32_t utc);

// Local time = UTC + offset (+1 h if euDst and summer time).
DateTime toLocal(uint32_t utc, int16_t utcOffsetMinutes, bool euDst);

}  // namespace hkv

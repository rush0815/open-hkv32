#pragma once
// Transfer schedule ("when to send"): either a fixed interval in minutes or a list of
// times of day. Text format used by the UI:
//   "15"                        -> every 15 minutes
//   "06:00 12:00 18:00 00:00"   -> daily at these local times (separators: space , ;)

#include <stddef.h>
#include <stdint.h>

#include "hkv/calendar.h"

namespace hkv {

struct TransferSchedule {
  enum Mode : uint8_t { Interval = 0, Times = 1 };
  static constexpr int kMaxTimes = 24;

  uint8_t mode = Interval;
  uint8_t count = 0;            // number of entries in times[]
  uint16_t intervalMin = 15;    // Interval mode: 1..1440
  uint16_t times[kMaxTimes] = {};  // Times mode: minutes of the day, sorted, unique
};

// nullptr = ok (out is replaced), otherwise a short English error text (out untouched).
const char* parseSchedule(const char* text, TransferSchedule& out);

// Canonical text form, e.g. "15" or "00:00 06:00 12:00 18:00".
void formatSchedule(const TransferSchedule& s, char* out, size_t cap);

// Minutes since 1970-01-01 in LOCAL time (monotonic index for time-of-day schedules).
int32_t localMinuteIndex(const DateTime& local);

// Times mode: first scheduled minute index strictly after afterIdx.
int32_t nextScheduledMinute(const TransferSchedule& s, int32_t afterIdx);

}  // namespace hkv

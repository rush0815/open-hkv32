#include "hkv/schedule.h"

#include <stdio.h>
#include <string.h>

namespace hkv {

namespace {
bool isSep(char c) { return c == ' ' || c == ',' || c == ';' || c == '\t' || c == '\n' || c == '\r'; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
}  // namespace

const char* parseSchedule(const char* text, TransferSchedule& out) {
  if (!text) return "empty schedule";
  while (isSep(*text)) ++text;
  if (!*text) return "empty schedule";

  // Plain number -> interval in minutes
  const char* p = text;
  uint32_t n = 0;
  int digits = 0;
  while (isDigit(*p) && digits < 6) {
    n = n * 10 + static_cast<uint32_t>(*p - '0');
    ++p;
    ++digits;
  }
  const char* rest = p;
  while (isSep(*rest)) ++rest;
  if (digits > 0 && *rest == '\0' && *p != ':') {
    if (n < 1 || n > 1440) return "interval must be 1..1440 minutes";
    TransferSchedule s;
    s.mode = TransferSchedule::Interval;
    s.intervalMin = static_cast<uint16_t>(n);
    out = s;
    return nullptr;
  }

  // List of hh:mm
  TransferSchedule s;
  s.mode = TransferSchedule::Times;
  p = text;
  while (*p) {
    while (isSep(*p)) ++p;
    if (!*p) break;
    int h = 0, m = 0, hd = 0, md = 0;
    while (isDigit(*p) && hd < 3) h = h * 10 + (*p++ - '0'), ++hd;
    if (hd == 0 || *p != ':') return "times must look like hh:mm";
    ++p;
    while (isDigit(*p) && md < 3) m = m * 10 + (*p++ - '0'), ++md;
    if (md != 2 || (*p && !isSep(*p))) return "times must look like hh:mm";
    if (h == 24 && m == 0) h = 0;  // 24:00 = midnight
    if (h > 23 || m > 59) return "invalid time of day";
    const uint16_t v = static_cast<uint16_t>(h * 60 + m);
    bool dup = false;
    for (int i = 0; i < s.count; ++i) dup |= s.times[i] == v;
    if (dup) continue;
    if (s.count >= TransferSchedule::kMaxTimes) return "at most 24 times";
    // insert sorted
    int i = s.count;
    while (i > 0 && s.times[i - 1] > v) {
      s.times[i] = s.times[i - 1];
      --i;
    }
    s.times[i] = v;
    ++s.count;
  }
  if (s.count == 0) return "empty schedule";
  out = s;
  return nullptr;
}

void formatSchedule(const TransferSchedule& s, char* out, size_t cap) {
  if (cap == 0) return;
  out[0] = '\0';
  if (s.mode == TransferSchedule::Interval) {
    snprintf(out, cap, "%u", static_cast<unsigned>(s.intervalMin));
    return;
  }
  size_t o = 0;
  for (int i = 0; i < s.count && o + 6 < cap; ++i) {
    o += static_cast<size_t>(snprintf(out + o, cap - o, "%s%02u:%02u", i ? " " : "", s.times[i] / 60u, s.times[i] % 60u));
  }
}

int32_t localMinuteIndex(const DateTime& local) {
  return daysFromCivil(local.year, local.month, local.day) * 1440 + local.hour * 60 + local.minute;
}

int32_t nextScheduledMinute(const TransferSchedule& s, int32_t afterIdx) {
  if (s.mode != TransferSchedule::Times || s.count == 0) return afterIdx + s.intervalMin;
  const int32_t day = afterIdx / 1440;
  const int32_t mod = afterIdx % 1440;
  for (int i = 0; i < s.count; ++i)
    if (s.times[i] > mod) return day * 1440 + s.times[i];
  return (day + 1) * 1440 + s.times[0];
}

}  // namespace hkv

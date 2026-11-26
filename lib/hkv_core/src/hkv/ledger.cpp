#include "hkv/ledger.h"

#include <string.h>

namespace hkv {

void Ledger::reset(MeterData& d) {
  memset(&d, 0, sizeof(d));
  d.lastPeriodYear = -1;
  d.lastMonthKey = -1;
}

void Ledger::accumulate(float unitsPerHour, float seconds) {
  if (unitsPerHour <= 0.0f || seconds <= 0.0f) return;
  fractionMilli_ += unitsPerHour * seconds / 3.6f;  // milli-units
  if (fractionMilli_ >= 1.0f) {
    const uint32_t whole = static_cast<uint32_t>(fractionMilli_);
    fractionMilli_ -= static_cast<float>(whole);
    d_.currentMilliUnits += whole;
    d_.lifetimeMilliUnits += whole;
  }
}

int32_t Ledger::periodYear(const DateTime& local, uint8_t dueMonth, uint8_t dueDay) {
  // The billing period labelled Y ends at the end of (Y, dueMonth, dueDay).
  const bool afterDue = local.month > dueMonth || (local.month == dueMonth && local.day > dueDay);
  return static_cast<int32_t>(local.year) + (afterDue ? 1 : 0);
}

DateTime Ledger::periodStart(const DateTime& local, uint8_t dueMonth, uint8_t dueDay) {
  const int32_t previousEnd = periodYear(local, dueMonth, dueDay) - 1;
  const int32_t days = daysFromCivil(previousEnd, dueMonth, dueDay) + 1;
  return fromUnix(static_cast<uint32_t>(days) * 86400u);
}

void Ledger::pushMonthly(int32_t monthKey, uint32_t units) {
  MonthlyValue& slot = d_.monthly[d_.monthlyHead];
  slot.year = static_cast<uint16_t>(monthKey / 12);
  slot.month = static_cast<uint8_t>(monthKey % 12 + 1);
  slot.reserved = 0;
  slot.units = units;
  d_.monthlyHead = static_cast<uint8_t>((d_.monthlyHead + 1) % kMonthlyHistory);
}

bool Ledger::updateCalendar(const DateTime& local, const Settings& s) {
  const int32_t monthKey = static_cast<int32_t>(local.year) * 12 + (local.month - 1);
  const int32_t period = periodYear(local, s.dueMonth, s.dueDay);

  if (d_.lastPeriodYear < 0 || d_.lastMonthKey < 0) {
    d_.lastPeriodYear = period;
    d_.lastMonthKey = monthKey;
    return true;
  }

  bool changed = false;

  // Monthly value first, so the December value still contains the full period.
  if (monthKey > d_.lastMonthKey) {
    pushMonthly(d_.lastMonthKey, currentUnits());
    d_.lastMonthKey = monthKey;
    changed = true;
  } else if (monthKey < d_.lastMonthKey) {
    d_.lastMonthKey = monthKey;  // clock moved backwards: resynchronise silently
    changed = true;
  }

  if (period > d_.lastPeriodYear) {
    d_.dueDateUnits = currentUnits();
    d_.dueDateMilliUnits = d_.currentMilliUnits;
    d_.dueDateYear = static_cast<uint16_t>(d_.lastPeriodYear);
    d_.dueDateMonth = s.dueMonth;
    d_.dueDateDay = s.dueDay;
    d_.currentMilliUnits = 0;
    fractionMilli_ = 0.0f;
    d_.offsetMilliUnits = 0;  // the offset is contained in the due date value now
    d_.offsetYear = 0;
    d_.offsetMonth = 0;
    d_.offsetDay = 0;
    d_.lastPeriodYear = period;
    changed = true;
  } else if (period < d_.lastPeriodYear) {
    d_.lastPeriodYear = period;
    changed = true;
  }
  return changed;
}

void Ledger::resetCurrent() {
  d_.currentMilliUnits = 0;
  fractionMilli_ = 0.0f;
  d_.offsetMilliUnits = 0;
  d_.offsetYear = 0;
  d_.offsetMonth = 0;
  d_.offsetDay = 0;
}

void Ledger::setOffset(int64_t milliUnits, const DateTime* date) {
  int64_t v = static_cast<int64_t>(d_.currentMilliUnits) - d_.offsetMilliUnits + milliUnits;
  if (v < 0) v = 0;
  d_.currentMilliUnits = static_cast<uint64_t>(v);
  d_.offsetMilliUnits = milliUnits;
  d_.offsetYear = (milliUnits != 0 && date) ? date->year : 0;
  d_.offsetMonth = (milliUnits != 0 && date) ? date->month : 0;
  d_.offsetDay = (milliUnits != 0 && date) ? date->day : 0;
}

bool Ledger::deleteMonthly(uint16_t year, uint8_t month) {
  bool found = false;
  for (MonthlyValue& m : d_.monthly) {
    if (m.year == year && m.month == month) {
      m = MonthlyValue{};
      found = true;
    }
  }
  return found;
}

bool Ledger::monthlyValue(int ageIndex, MonthlyValue& out) const {
  if (ageIndex < 0 || ageIndex >= kMonthlyHistory) return false;
  const int idx = (d_.monthlyHead - 1 - ageIndex + 2 * kMonthlyHistory) % kMonthlyHistory;
  out = d_.monthly[idx];
  return out.year != 0;
}

}  // namespace hkv

#pragma once
// Consumption ledger: accumulates units, handles due date (Stichtag) and monthly values.
// MeterData is a plain struct that is persisted as a blob (see persist.h).

#include <stdint.h>

#include "hkv/calendar.h"
#include "hkv/settings.h"

namespace hkv {

constexpr int kMonthlyHistory = 18;

struct MonthlyValue {
  uint16_t year;   // 0 = empty slot
  uint8_t month;
  uint8_t reserved;
  uint32_t units;  // cumulative units of the billing period at the end of that month
};

struct MeterData {
  uint64_t currentMilliUnits;    // since last due date
  uint64_t lifetimeMilliUnits;   // never reset (diagnostics)
  uint32_t dueDateUnits;         // value at last due date (whole units, OMS)
  uint64_t dueDateMilliUnits;    // same with decimals (display)
  uint16_t dueDateYear;          // 0 = no due date reached yet
  uint8_t dueDateMonth;
  uint8_t dueDateDay;
  int32_t lastPeriodYear;        // -1 = unknown (time never valid)
  int32_t lastMonthKey;          // year*12 + month-1, -1 = unknown
  MonthlyValue monthly[kMonthlyHistory];  // ring buffer
  uint8_t monthlyHead;           // index of the next slot to write
  uint8_t accessNo;              // wM-Bus access number
  uint16_t reserved;
  uint32_t errorHistory;         // sticky error flags (OR of all errors ever seen)
  // Counter offset, e.g. the reading of a replaced device. It is part of
  // currentMilliUnits; stored separately so it can be changed or removed later.
  int64_t offsetMilliUnits;
  uint16_t offsetYear;           // 0 = date unknown (clock not set when applied)
  uint8_t offsetMonth;
  uint8_t offsetDay;
  // --- added in data version 4 (keep new fields at the end) ---
  uint32_t lifetimeMeasurements;  // measurements since commissioning / last lifetime reset
};

class Ledger {
 public:
  explicit Ledger(MeterData& d) : d_(d) {}

  static void reset(MeterData& d);

  // Adds rate [units/h] * seconds. Fractions below 1/1000 unit are carried in RAM.
  void accumulate(float unitsPerHour, float seconds);

  // Applies month / due date rollovers for the given LOCAL time.
  // Returns true if persistent data changed in a way that should be saved immediately.
  bool updateCalendar(const DateTime& local, const Settings& s);

  uint32_t currentUnits() const { return static_cast<uint32_t>(d_.currentMilliUnits / 1000u); }

  // Current billing period back to 0 (incl. offset). Due date and monthly values stay.
  void resetCurrent();
  // Sets (replaces) the counter offset; the current value changes by the difference.
  // date may be nullptr if the clock is not set. The result never goes below 0.
  void setOffset(int64_t milliUnits, const DateTime* date);
  // Deletes the stored value of one month. Returns false if there is none.
  bool deleteMonthly(uint16_t year, uint8_t month);

  // Newest first; returns false if the slot is empty.
  bool monthlyValue(int ageIndex, MonthlyValue& out) const;

  static int32_t periodYear(const DateTime& local, uint8_t dueMonth, uint8_t dueDay);
  // First day of the billing period that contains 'local'.
  static DateTime periodStart(const DateTime& local, uint8_t dueMonth, uint8_t dueDay);

 private:
  void pushMonthly(int32_t monthKey, uint32_t units);

  MeterData& d_;
  float fractionMilli_ = 0.0f;
};

}  // namespace hkv

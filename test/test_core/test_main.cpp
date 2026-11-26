// Native unit tests for the portable core:  pio test -e native
#include <stddef.h>
#include <string.h>
#include <unity.h>

#include <map>
#include <string>
#include <vector>

#include "hkv/aes128.h"
#include "hkv/allocator.h"
#include "hkv/app.h"
#include "hkv/calendar.h"
#include "hkv/ledger.h"
#include "hkv/oms.h"
#include "hkv/persist.h"
#include "hkv/schedule.h"
#include "hkv/settings.h"
#include "hkv/sim/emulated_radiator.h"

using namespace hkv;

// ---------- fakes ----------
class FakeClock : public IClock {
 public:
  uint32_t utc = 0;
  bool valid = false;
  uint32_t ms = 0;
  uint32_t nowUtc() override { return utc + ms / 1000; }
  bool isTimeValid() override { return valid; }
  void setUtc(uint32_t t) override {
    utc = t - ms / 1000;
    valid = true;
  }
  uint32_t uptimeMs() override { return ms; }
};

class FakeStorage : public IStorage {
 public:
  std::map<std::string, std::vector<uint8_t>> kv;
  bool read(const char* key, void* buf, size_t len) override {
    auto it = kv.find(key);
    if (it == kv.end() || it->second.size() != len) return false;
    memcpy(buf, it->second.data(), len);
    return true;
  }
  bool write(const char* key, const void* buf, size_t len) override {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    kv[key] = std::vector<uint8_t>(p, p + len);
    return true;
  }
  bool erase(const char* key) override { return kv.erase(key) > 0; }
};

class FixedSensor : public ITemperatureSensor {
 public:
  TempReading r;
  explicit FixedSensor(float c) { r.valid = true; r.celsius = c; }
  bool begin() override { return true; }
  TempReading read() override { return r; }
  const char* name() const override { return "fixed"; }
};

class FakeBattery : public IBattery {
 public:
  bool begin() override { return true; }
  BatteryStatus read() override {
    BatteryStatus b;
    b.valid = true;
    b.millivolts = 3900;
    b.percent = 70;
    return b;
  }
};

class CaptureSink : public ITelegramSink {
 public:
  std::vector<uint8_t> last;
  int count = 0;
  void send(const uint8_t* f, size_t n) override {
    last.assign(f, f + n);
    ++count;
  }
};

static Settings defaults() {
  Settings s;
  setDefaults(s, 12345678);
  return s;
}

// ---------- calendar ----------
void test_calendar_roundtrip() {
  const uint32_t t = 1767225599;  // 2025-12-31 23:59:59 UTC
  DateTime d = fromUnix(t);
  TEST_ASSERT_EQUAL(2025, d.year);
  TEST_ASSERT_EQUAL(12, d.month);
  TEST_ASSERT_EQUAL(31, d.day);
  TEST_ASSERT_EQUAL(23, d.hour);
  TEST_ASSERT_EQUAL(3, d.weekday);  // Wednesday
  TEST_ASSERT_EQUAL_UINT32(t, toUnix(d));
  TEST_ASSERT_EQUAL(29, daysInMonth(2028, 2));
  TEST_ASSERT_EQUAL(28, daysInMonth(2100, 2));
}

void test_eu_dst() {
  // 2026: summer time 29.03. 01:00 UTC .. 25.10. 01:00 UTC
  TEST_ASSERT_FALSE(isEuSummerTime(1774745999));  // 2026-03-29 00:59:59 UTC
  TEST_ASSERT_TRUE(isEuSummerTime(1774746000));   // 2026-03-29 01:00:00 UTC
  TEST_ASSERT_TRUE(isEuSummerTime(1792889999));   // 2026-10-25 00:59:59 UTC
  TEST_ASSERT_FALSE(isEuSummerTime(1792890000));  // 2026-10-25 01:00:00 UTC
  DateTime l = toLocal(1774746000, 60, true);
  TEST_ASSERT_EQUAL(3, l.hour);
  TEST_ASSERT_TRUE(l.dst);
}

// ---------- allocator ----------
void test_allocator_thresholds() {
  Settings s = defaults();
  TempReading room{true, 20.0f};
  TEST_ASSERT_EQUAL(CountState::Idle, evaluate(s, {true, 23.0f}, room, false).state);
  TEST_ASSERT_EQUAL(CountState::SensorError, evaluate(s, {false, 0}, room, false).state);
  AllocResult r = evaluate(s, {true, 70.0f}, room, false);
  TEST_ASSERT_EQUAL(CountState::Counting, r.state);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, r.unitsPerHour);  // dT = 50 K = nominal -> Kc
  TEST_ASSERT_EQUAL(CountState::Summer, evaluate(s, {true, 35.0f}, room, true).state);
  s.scale = ScaleMode::Product;
  s.kq = 1.5f;
  s.kc = 0.8f;
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.2f, evaluate(s, {true, 70.0f}, room, false).unitsPerHour);
  // room sensor failure -> fallback 20 C
  AllocResult f = evaluate(s, {true, 45.0f}, {false, 0}, false);
  TEST_ASSERT_TRUE(f.roomFallback);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 25.0f, f.deltaK);
}

// ---------- ledger ----------
void test_ledger_accumulate() {
  MeterData d;
  Ledger::reset(d);
  Ledger l(d);
  for (int i = 0; i < 3600; ++i) l.accumulate(2.0f, 1.0f);  // 2 units/h for 1 h in 1 s steps
  TEST_ASSERT_EQUAL_UINT32(2, l.currentUnits());
  TEST_ASSERT_UINT64_WITHIN(2, 2000, d.currentMilliUnits);
}

void test_ledger_due_date_and_months() {
  Settings s = defaults();  // due date 31.12.
  MeterData d;
  Ledger::reset(d);
  Ledger l(d);
  DateTime t;
  t.year = 2025; t.month = 11; t.day = 30;
  l.updateCalendar(t, s);
  d.currentMilliUnits = 100000;  // 100 units
  t.month = 12; t.day = 1;
  TEST_ASSERT_TRUE(l.updateCalendar(t, s));
  MonthlyValue mv;
  TEST_ASSERT_TRUE(l.monthlyValue(0, mv));
  TEST_ASSERT_EQUAL(2025, mv.year);
  TEST_ASSERT_EQUAL(11, mv.month);
  TEST_ASSERT_EQUAL_UINT32(100, mv.units);
  d.currentMilliUnits = 250000;
  t.day = 31;
  TEST_ASSERT_FALSE(l.updateCalendar(t, s));  // still inside the period
  t.year = 2026; t.month = 1; t.day = 1;
  TEST_ASSERT_TRUE(l.updateCalendar(t, s));
  TEST_ASSERT_EQUAL_UINT32(250, d.dueDateUnits);
  TEST_ASSERT_EQUAL(2025, d.dueDateYear);
  TEST_ASSERT_EQUAL(12, d.dueDateMonth);
  TEST_ASSERT_EQUAL(31, d.dueDateDay);
  TEST_ASSERT_EQUAL_UINT32(0, l.currentUnits());
  TEST_ASSERT_TRUE(l.monthlyValue(0, mv));
  TEST_ASSERT_EQUAL(12, mv.month);
  TEST_ASSERT_EQUAL_UINT32(250, mv.units);
}

void test_period_start() {
  DateTime t;
  t.year = 2026; t.month = 11; t.day = 24;
  DateTime p = Ledger::periodStart(t, 12, 31);
  TEST_ASSERT_EQUAL(2026, p.year); TEST_ASSERT_EQUAL(1, p.month); TEST_ASSERT_EQUAL(1, p.day);
  p = Ledger::periodStart(t, 8, 31);
  TEST_ASSERT_EQUAL(2026, p.year); TEST_ASSERT_EQUAL(9, p.month); TEST_ASSERT_EQUAL(1, p.day);
  t.month = 2; t.day = 28;
  p = Ledger::periodStart(t, 8, 31);
  TEST_ASSERT_EQUAL(2025, p.year); TEST_ASSERT_EQUAL(9, p.month); TEST_ASSERT_EQUAL(1, p.day);
}

void test_offset_and_monthly_delete() {
  MeterData d;
  Ledger::reset(d);
  Ledger l(d);
  d.currentMilliUnits = 400;                 // 0.4 units counted by the new device
  DateTime t;
  t.year = 2026; t.month = 11; t.day = 24;
  l.setOffset(57300, &t);                    // old device showed 57.3
  TEST_ASSERT_EQUAL_UINT64(57700, d.currentMilliUnits);
  TEST_ASSERT_EQUAL(2026, d.offsetYear);
  l.setOffset(50000, &t);                    // corrected offset replaces the old one
  TEST_ASSERT_EQUAL_UINT64(50400, d.currentMilliUnits);
  l.setOffset(0, &t);                        // removed
  TEST_ASSERT_EQUAL_UINT64(400, d.currentMilliUnits);
  TEST_ASSERT_EQUAL(0, d.offsetYear);

  Settings s = defaults();
  t.month = 10; t.day = 31;
  l.updateCalendar(t, s);
  t.month = 11; t.day = 1;
  l.updateCalendar(t, s);                    // October value stored
  MonthlyValue mv;
  TEST_ASSERT_TRUE(l.monthlyValue(0, mv));
  TEST_ASSERT_FALSE(l.deleteMonthly(2026, 9));
  TEST_ASSERT_TRUE(l.deleteMonthly(2026, 10));
  TEST_ASSERT_FALSE(l.monthlyValue(0, mv));

  d.offsetMilliUnits = 1000;
  l.resetCurrent();
  TEST_ASSERT_EQUAL_UINT64(0, d.currentMilliUnits);
  TEST_ASSERT_EQUAL_INT64(0, d.offsetMilliUnits);
}

// ---------- persistence ----------
void test_persist_crc() {
  FakeStorage st;
  Settings s = defaults();
  TEST_ASSERT_TRUE(saveObject(st, "settings", 1, s));
  Settings r;
  TEST_ASSERT_TRUE(loadObject(st, "settings", 1, r));
  TEST_ASSERT_EQUAL_MEMORY(&s, &r, sizeof(s));
  TEST_ASSERT_FALSE(loadObject(st, "settings", 2, r));  // version mismatch
  st.kv["settings"][20] ^= 0x01;                       // corrupt
  TEST_ASSERT_FALSE(loadObject(st, "settings", 1, r));
}

// ---------- crypto / OMS ----------
void test_aes_fips197() {
  const uint8_t key[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  const uint8_t pt[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                          0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
  const uint8_t ct[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
                          0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};
  SoftAes128 aes;
  aes.setKey(key);
  uint8_t out[16];
  aes.encryptBlock(pt, out);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(ct, out, 16);
}

void test_crc16_check_value() {
  const uint8_t s[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  TEST_ASSERT_EQUAL_HEX16(0xC2B7, oms::crc16(s, sizeof(s)));  // CRC-16/EN-13757
}

void test_oms_unencrypted_frame() {
  oms::HcaTelegram t;
  t.manufacturer = "XXX";
  t.meterId = 12345678;
  t.accessNo = 0x10;
  t.currentUnits = 1234;
  uint8_t f[oms::kMaxFrame];
  const size_t n = oms::buildHcaTelegram(t, nullptr, f, sizeof(f));
  const uint8_t expected[] = {0x13, 0x44, 0x18, 0x63, 0x78, 0x56, 0x34, 0x12, 0x01, 0x08,
                              0x7A, 0x10, 0x00, 0x00, 0x00, 0x03, 0x6E, 0xD2, 0x04, 0x00};
  TEST_ASSERT_EQUAL(sizeof(expected), n);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, f, n);
  TEST_ASSERT_EQUAL_HEX16(0x6318, oms::manufacturerCode("XXX"));
}

void test_oms_mode5_length() {
  oms::HcaTelegram t;
  const uint8_t key[16] = {0};
  t.security = 5;
  t.aesKey = key;
  t.currentUnits = 1;
  SoftAes128 aes;
  uint8_t f[oms::kMaxFrame];
  const size_t n = oms::buildHcaTelegram(t, &aes, f, sizeof(f));
  TEST_ASSERT_EQUAL(15 + 16, n);   // header + one encrypted block
  TEST_ASSERT_EQUAL_HEX8(0x10, f[13]);  // N = 1 block
  TEST_ASSERT_EQUAL_HEX8(0x05, f[14]);  // mode 5
  TEST_ASSERT_EQUAL(0, oms::buildHcaTelegram(t, nullptr, f, sizeof(f)));  // cipher required
}

void test_date_codes() {
  DateTime d;
  d.year = 2025; d.month = 12; d.day = 31;
  // year 25 = 0b0011001: low3 = 001, high4 = 0011
  TEST_ASSERT_EQUAL_HEX16((31) | (1 << 5) | (12 << 8) | (3 << 12), oms::encodeDateG(d));
}

// ---------- emulator ----------
void test_emulator_range() {
  FakeClock c;
  XorShiftRandom rnd(42);
  EmulatedRadiatorSensor e(c, rnd);
  e.begin();
  float lo = 100, hi = -100;
  for (int i = 0; i < 24 * 30; ++i) {  // 24 h in 2 min steps
    c.ms += 120000;
    TempReading r = e.read();
    TEST_ASSERT_TRUE(r.valid);
    if (r.celsius < lo) lo = r.celsius;
    if (r.celsius > hi) hi = r.celsius;
  }
  TEST_ASSERT_TRUE(lo > 18.0f && lo < 25.0f);
  TEST_ASSERT_TRUE(hi > 35.0f && hi < 63.0f);
}

// ---------- full app ----------
void test_app_counts_and_sends() {
  FakeClock clock;
  FixedSensor rad(70.0f), room(20.0f);
  FakeBattery bat;
  FakeStorage st;
  CaptureSink sink;
  SoftAes128 aes;
  Hal hal{clock, rad, room, bat, st, nullptr, &aes, &sink};
  HkvApp app(hal);
  app.begin(87654321);
  clock.setUtc(1764547200);  // 2025-12-01 00:00 UTC
  for (int i = 0; i < 2 * 3600; ++i) {  // 2 h in 1 s steps
    clock.ms += 1000;
    app.poll();
  }
  TEST_ASSERT_UINT32_WITHIN(1, 2, app.currentUnits());  // dT 50 K -> 1 unit/h
  TEST_ASSERT_TRUE(sink.count >= 25);
  TEST_ASSERT_EQUAL_HEX8(0x08, sink.last[9]);
  TEST_ASSERT_EQUAL(0u, app.snapshot().errors);

  // data survives a "reboot"
  HkvApp app2(hal);
  app2.begin(1);
  TEST_ASSERT_EQUAL_UINT32(87654321, app2.settings().meterId);
}

void test_live_mode_does_not_count() {
  FakeClock clock;
  FixedSensor rad(70.0f), room(20.0f);
  FakeBattery bat;
  FakeStorage st;
  Hal hal{clock, rad, room, bat, st, nullptr, nullptr, nullptr};
  HkvApp app(hal);
  app.begin(1);
  clock.ms += 1000;
  app.poll();  // regular first measurement
  const uint32_t meas = app.snapshot().measurements;
  const uint64_t milli = app.meterData().currentMilliUnits;
  app.keepLiveAlive();
  TEST_ASSERT_TRUE(app.liveActive());
  TEST_ASSERT_TRUE(app.live().valid);
  rad.r.celsius = 55.0f;
  for (int i = 0; i < 50; ++i) {  // 5 s in 100 ms steps, keep-alive like the web UI
    clock.ms += 100;
    if (i % 20 == 0) app.keepLiveAlive();
    app.poll();
  }
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 55.0f, app.live().alloc.radiatorC);   // live value updated
  TEST_ASSERT_EQUAL_UINT32(meas, app.snapshot().measurements);         // no extra real measurement
  TEST_ASSERT_EQUAL_UINT64(milli, app.meterData().currentMilliUnits);  // no extra units
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 70.0f, app.snapshot().alloc.radiatorC);
  clock.ms += 7000;
  app.poll();
  TEST_ASSERT_FALSE(app.liveActive());  // expires without keep-alive
}

// Same layout as MeterData version 2 (before the counter offset existed).
struct MeterDataV2Test {
  uint64_t currentMilliUnits;
  uint64_t lifetimeMilliUnits;
  uint32_t dueDateUnits;
  uint64_t dueDateMilliUnits;
  uint16_t dueDateYear;
  uint8_t dueDateMonth;
  uint8_t dueDateDay;
  int32_t lastPeriodYear;
  int32_t lastMonthKey;
  MonthlyValue monthly[kMonthlyHistory];
  uint8_t monthlyHead;
  uint8_t accessNo;
  uint16_t reserved;
  uint32_t errorHistory;
};

void test_meter_data_migration_v2() {
  FakeClock clock;
  FixedSensor rad(20.0f), room(20.0f);
  FakeBattery bat;
  FakeStorage st;
  MeterDataV2Test old;
  memset(&old, 0, sizeof(old));
  old.currentMilliUnits = 123456;
  old.lifetimeMilliUnits = 999000;
  old.dueDateUnits = 118;
  old.dueDateMilliUnits = 118700;
  old.dueDateYear = 2025;
  old.dueDateMonth = 12;
  old.dueDateDay = 31;
  old.lastPeriodYear = 2026;
  old.lastMonthKey = 2026 * 12 + 10;
  old.monthly[0].year = 2026;
  old.monthly[0].month = 10;
  old.monthly[0].units = 96;
  old.monthlyHead = 1;
  old.accessNo = 42;
  TEST_ASSERT_TRUE(saveObject(st, "meter", 2, old));

  Hal hal{clock, rad, room, bat, st, nullptr, nullptr, nullptr};
  HkvApp app(hal);
  app.begin(1);
  const MeterData& d = app.meterData();
  TEST_ASSERT_EQUAL_UINT64(123456, d.currentMilliUnits);
  TEST_ASSERT_EQUAL_UINT64(999000, d.lifetimeMilliUnits);
  TEST_ASSERT_EQUAL_UINT64(118700, d.dueDateMilliUnits);
  TEST_ASSERT_EQUAL(2025, d.dueDateYear);
  TEST_ASSERT_EQUAL(42, d.accessNo);
  TEST_ASSERT_EQUAL_INT64(0, d.offsetMilliUnits);
  MonthlyValue mv;
  TEST_ASSERT_TRUE(app.monthlyValue(0, mv));
  TEST_ASSERT_EQUAL_UINT32(96, mv.units);
  MeterData reloaded;
  TEST_ASSERT_TRUE(loadObject(st, "meter", HkvApp::kDataVersion, reloaded));  // saved in the new format
}

void test_meter_data_migration_v3() {
  FakeClock clock;
  FixedSensor rad(20.0f), room(20.0f);
  FakeBattery bat;
  FakeStorage st;
  MeterData v3;
  Ledger::reset(v3);
  v3.currentMilliUnits = 154066;
  v3.offsetMilliUnits = 154000;
  v3.offsetYear = 2026;
  TEST_ASSERT_TRUE(saveObject(st, "meter", 3, v3));  // same size, older version number
  Hal hal{clock, rad, room, bat, st, nullptr, nullptr, nullptr};
  HkvApp app(hal);
  app.begin(1);
  TEST_ASSERT_EQUAL_UINT64(154066, app.meterData().currentMilliUnits);
  TEST_ASSERT_EQUAL_INT64(154000, app.meterData().offsetMilliUnits);
  TEST_ASSERT_EQUAL_UINT32(0, app.meterData().lifetimeMeasurements);
  clock.ms += 1000;
  app.poll();
  TEST_ASSERT_EQUAL_UINT32(1, app.meterData().lifetimeMeasurements);
  app.resetLifetime();
  TEST_ASSERT_EQUAL_UINT32(0, app.meterData().lifetimeMeasurements);
}

void test_factory_reset_blocks_writes() {
  FakeClock clock;
  FixedSensor rad(70.0f), room(20.0f);
  FakeBattery bat;
  FakeStorage st;
  Hal hal{clock, rad, room, bat, st, nullptr, nullptr, nullptr};
  HkvApp app(hal);
  app.begin(1);
  TEST_ASSERT_TRUE(st.kv.count("settings") && st.kv.count("meter"));
  app.eraseStoredData();
  TEST_ASSERT_FALSE(st.kv.count("settings") || st.kv.count("meter"));
  app.resetCurrentPeriod();                       // would save
  app.resetLifetime();
  TEST_ASSERT_NOT_NULL(app.applySettings(app.settings()));
  for (int i = 0; i < 3; ++i) {
    clock.ms += 3600000;
    app.poll();
  }
  TEST_ASSERT_FALSE(st.kv.count("settings") || st.kv.count("meter"));
}

void test_transfer_schedule() {
  TransferSchedule t;
  char buf[160];
  TEST_ASSERT_NULL(parseSchedule(" 15 ", t));
  TEST_ASSERT_EQUAL(TransferSchedule::Interval, t.mode);
  TEST_ASSERT_EQUAL(15, t.intervalMin);
  formatSchedule(t, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("15", buf);

  TEST_ASSERT_NULL(parseSchedule("06:00 12:00, 18:00;00:00 12:00", t));
  TEST_ASSERT_EQUAL(TransferSchedule::Times, t.mode);
  TEST_ASSERT_EQUAL(4, t.count);  // duplicate 12:00 removed
  formatSchedule(t, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("00:00 06:00 12:00 18:00", buf);
  TEST_ASSERT_NULL(parseSchedule("24:00", t));
  TEST_ASSERT_EQUAL(0, t.times[0]);

  TransferSchedule keep = t;
  TEST_ASSERT_NOT_NULL(parseSchedule("", t));
  TEST_ASSERT_NOT_NULL(parseSchedule("0", t));
  TEST_ASSERT_NOT_NULL(parseSchedule("1441", t));
  TEST_ASSERT_NOT_NULL(parseSchedule("6:00 abc", t));
  TEST_ASSERT_NOT_NULL(parseSchedule("25:00", t));
  TEST_ASSERT_NOT_NULL(parseSchedule("12:5", t));
  TEST_ASSERT_NOT_NULL(parseSchedule("15 30", t));
  TEST_ASSERT_EQUAL_MEMORY(&keep, &t, sizeof(t));  // untouched on error

  // next due: today 13:37 -> 18:00 today; 23:10 -> 00:00 tomorrow
  TEST_ASSERT_NULL(parseSchedule("06:00 12:00 18:00 00:00", t));
  DateTime d;
  d.year = 2026; d.month = 11; d.day = 25; d.hour = 13; d.minute = 37;
  const int32_t now = localMinuteIndex(d);
  TEST_ASSERT_EQUAL_INT32(now - (13 * 60 + 37) + 18 * 60, nextScheduledMinute(t, now));
  d.hour = 23; d.minute = 10;
  const int32_t late = localMinuteIndex(d);
  TEST_ASSERT_EQUAL_INT32(late - (23 * 60 + 10) + 1440, nextScheduledMinute(t, late));
  d.hour = 18; d.minute = 0;  // exactly on a slot -> the next one
  TEST_ASSERT_EQUAL_INT32(localMinuteIndex(d) + 6 * 60, nextScheduledMinute(t, localMinuteIndex(d)));
}

void test_emissivity_correction() {
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 50.0f, correctEmissivity(50.0f, 25.0f, 1.0f));   // no correction
  const float t = correctEmissivity(50.0f, 25.0f, 0.95f);                           // painted radiator
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 51.18f, t);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 25.0f, correctEmissivity(25.0f, 25.0f, 0.95f));  // same as sensor: no effect
}

void test_settings_migration_v1() {
  FakeClock clock;
  FixedSensor rad(20.0f), room(20.0f);
  FakeBattery bat;
  FakeStorage st;
  Settings v1;
  setDefaults(v1, 11112222);
  v1.kc = 0.8f;
  TEST_ASSERT_TRUE(saveBlob(st, "settings", 1, &v1, offsetof(Settings, emissivity)));
  Hal hal{clock, rad, room, bat, st, nullptr, nullptr, nullptr};
  HkvApp app(hal);
  app.begin(1);
  TEST_ASSERT_EQUAL_UINT32(11112222, app.settings().meterId);  // kept, not reset to defaults
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.8f, app.settings().kc);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, app.settings().emissivity);
}

void test_room_coupling() {
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 34.0f, correctRoomCoupling(34.0f, 52.0f, 0.0f));   // off
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 22.0f, correctRoomCoupling(34.0f, 52.0f, 0.4f));   // (34-20.8)/0.6
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.4f, couplingFromReference(34.0f, 52.0f, 22.0f));
  TEST_ASSERT_TRUE(couplingFromReference(24.0f, 25.0f, 22.0f) < 0.0f);              // radiator too cold
  Settings s = defaults();
  s.roomCoupling = 0.4f;
  AllocResult r = evaluate(s, {true, 52.0f}, {true, 34.0f}, false);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 22.0f, r.roomC);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 30.0f, r.deltaK);
}

void test_settings_migration_v3() {
  FakeClock clock;
  FixedSensor rad(20.0f), room(20.0f);
  FakeBattery bat;
  FakeStorage st;
  Settings v3;
  setDefaults(v3, 55556666);
  v3.displayInvert = 0;
  v3.emissivity = 0.95f;
  TEST_ASSERT_TRUE(saveBlob(st, "settings", 3, &v3, offsetof(Settings, roomCoupling)));
  Hal hal{clock, rad, room, bat, st, nullptr, nullptr, nullptr};
  HkvApp app(hal);
  app.begin(1);
  TEST_ASSERT_EQUAL_UINT32(55556666, app.settings().meterId);
  TEST_ASSERT_EQUAL(0, app.settings().displayInvert);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.95f, app.settings().emissivity);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, app.settings().roomCoupling);
}

void test_room_coupling_lag() {
  // Room sensor model: lags the radiator with tau = 10 min, coupling c = 0.4, room 21 C.
  FakeClock clock;
  FixedSensor rad(30.0f), room(0.0f);
  FakeBattery bat;
  FakeStorage st;
  Hal hal{clock, rad, room, bat, st, nullptr, nullptr, nullptr};
  HkvApp app(hal);
  app.begin(1);
  Settings s = app.settings();
  s.roomCoupling = 0.4f;
  s.roomCouplingTauMin = 10.0f;
  s.measureIntervalS = 60;
  TEST_ASSERT_NULL(app.applySettings(s));
  const float c = 0.4f, ta = 21.0f, tau = 600.0f;
  float sensorLag = 30.0f;  // the physical sensor's own lagged view of the radiator
  room.r.celsius = ta + c * (sensorLag - ta);  // settled state at 30 C before the cycles start
  clock.ms += 60000;
  app.poll();
  float worst = 0.0f;
  for (int i = 0; i < 180; ++i) {  // 3 h, radiator heats up and cools down in 1 h cycles
    const int minute = i % 60;
    rad.r.celsius = minute < 15 ? 52.0f : 30.0f;
    sensorLag += (rad.r.celsius - sensorLag) * (1.0f - expf(-60.0f / tau));
    room.r.celsius = ta + c * (sensorLag - ta);
    clock.ms += 60000;
    app.poll();
    if (i > 5) {
      const float err = fabsf(app.snapshot().alloc.roomC - ta);
      if (err > worst) worst = err;
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(worst < 1.0f, "corrected room temperature must stay within 1 K");
  // without the lag (tau = 0) the same cycles cause several kelvin of error
  s.roomCouplingTauMin = 0.0f;
  TEST_ASSERT_NULL(app.applySettings(s));
  float worstNoLag = 0.0f;
  for (int i = 0; i < 120; ++i) {
    const int minute = i % 60;
    rad.r.celsius = minute < 15 ? 52.0f : 30.0f;
    sensorLag += (rad.r.celsius - sensorLag) * (1.0f - expf(-60.0f / tau));
    room.r.celsius = ta + c * (sensorLag - ta);
    clock.ms += 60000;
    app.poll();
    const float err = fabsf(app.snapshot().alloc.roomC - ta);
    if (err > worstNoLag) worstNoLag = err;
  }
  TEST_ASSERT_TRUE(worstNoLag > 3.0f);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_calendar_roundtrip);
  RUN_TEST(test_eu_dst);
  RUN_TEST(test_allocator_thresholds);
  RUN_TEST(test_ledger_accumulate);
  RUN_TEST(test_ledger_due_date_and_months);
  RUN_TEST(test_period_start);
  RUN_TEST(test_offset_and_monthly_delete);
  RUN_TEST(test_persist_crc);
  RUN_TEST(test_meter_data_migration_v2);
  RUN_TEST(test_meter_data_migration_v3);
  RUN_TEST(test_factory_reset_blocks_writes);
  RUN_TEST(test_transfer_schedule);
  RUN_TEST(test_emissivity_correction);
  RUN_TEST(test_settings_migration_v1);
  RUN_TEST(test_settings_migration_v3);
  RUN_TEST(test_room_coupling);
  RUN_TEST(test_room_coupling_lag);
  RUN_TEST(test_aes_fips197);
  RUN_TEST(test_crc16_check_value);
  RUN_TEST(test_oms_unencrypted_frame);
  RUN_TEST(test_oms_mode5_length);
  RUN_TEST(test_date_codes);
  RUN_TEST(test_emulator_range);
  RUN_TEST(test_app_counts_and_sends);
  RUN_TEST(test_live_mode_does_not_count);
  return UNITY_END();
}

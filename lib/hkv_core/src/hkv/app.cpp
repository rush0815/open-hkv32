#include "hkv/app.h"

#include <stddef.h>
#include <string.h>

#include "hkv/log.h"
#include "hkv/persist.h"

namespace hkv {

namespace {
constexpr const char* kKeySettings = "settings";
constexpr const char* kKeyData = "meter";
constexpr uint32_t kSaveMinIntervalMs = 15u * 60u * 1000u;  // flash wear: at most every 15 min
constexpr uint32_t kSaveMaxIntervalMs = 6u * 3600u * 1000u;
constexpr uint32_t kDisplayRefreshMs = 250;  // smooth progress animations

// Layout of MeterData version 2 (without counter offset) - for migration.
struct MeterDataV2 {
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

// true if a is at or after b (wrap-safe)
bool reached(uint32_t now, uint32_t deadline) { return static_cast<int32_t>(now - deadline) >= 0; }
}  // namespace

void HkvApp::begin(uint32_t defaultMeterId) {
  bool settingsOk = loadObject(hal_.storage, kKeySettings, kSettingsVersion, settings_);
  if (!settingsOk) {
    // Older versions have the identical layout, only without the fields appended later.
    // Missing fields keep their defaults.
    struct OldVersion {
      uint16_t version;
      size_t size;
    };
    const OldVersion olds[] = {{2, offsetof(Settings, displayInvert)}, {1, offsetof(Settings, emissivity)}};
    for (const OldVersion& o : olds) {
      Settings old;
      setDefaults(old, defaultMeterId);
      if (loadBlob(hal_.storage, kKeySettings, o.version, &old, o.size)) {
        settings_ = old;
        settingsOk = true;
        saveObject(hal_.storage, kKeySettings, kSettingsVersion, settings_);
        HKV_LOG("settings migrated (v%u -> v%u)", o.version, kSettingsVersion);
        break;
      }
    }
  }
  if (!settingsOk || validate(settings_) != nullptr) {
    HKV_LOG("settings: defaults");
    setDefaults(settings_, defaultMeterId);
    saveObject(hal_.storage, kKeySettings, kSettingsVersion, settings_);
  }
  MeterDataV2 v2;
  if (loadObject(hal_.storage, kKeyData, kDataVersion, data_)) {
    // current format
  } else if (loadObject(hal_.storage, kKeyData, 3, data_)) {
    static_assert((offsetof(MeterData, lifetimeMeasurements) + alignof(MeterData) - 1) / alignof(MeterData) *
                          alignof(MeterData) ==
                      sizeof(MeterData),
                  "v3 -> v4 migration assumes the new field fits into the former tail padding");
    // v3 has the same size: the new counter sits in former padding bytes
    data_.lifetimeMeasurements = 0;
    HKV_LOG("meter data migrated (v3 -> v4)");
    saveObject(hal_.storage, kKeyData, kDataVersion, data_);
  } else if (loadObject(hal_.storage, kKeyData, 2, v2)) {
    Ledger::reset(data_);
    data_.currentMilliUnits = v2.currentMilliUnits;
    data_.lifetimeMilliUnits = v2.lifetimeMilliUnits;
    data_.dueDateUnits = v2.dueDateUnits;
    data_.dueDateMilliUnits = v2.dueDateMilliUnits;
    data_.dueDateYear = v2.dueDateYear;
    data_.dueDateMonth = v2.dueDateMonth;
    data_.dueDateDay = v2.dueDateDay;
    data_.lastPeriodYear = v2.lastPeriodYear;
    data_.lastMonthKey = v2.lastMonthKey;
    memcpy(data_.monthly, v2.monthly, sizeof(data_.monthly));
    data_.monthlyHead = v2.monthlyHead;
    data_.accessNo = v2.accessNo;
    data_.errorHistory = v2.errorHistory;
    HKV_LOG("meter data migrated (v2 -> v3)");
    saveObject(hal_.storage, kKeyData, kDataVersion, data_);
  } else {
    HKV_LOG("meter data: none stored, starting at 0");
    Ledger::reset(data_);
    if (!saveObject(hal_.storage, kKeyData, kDataVersion, data_)) data_.errorHistory |= kErrStorage;
  }
  savedUnits_ = ledger_.currentUnits();

  hal_.radiator.begin();
  hal_.radiator.setEmissivity(settings_.emissivity);
  hal_.room.begin();
  hal_.battery.begin();
  if (hal_.display) {
    hal_.display->begin();
    hal_.display->configure(settings_.displayInvert != 0, settings_.displayFlip != 0);
  }
  if (hal_.display) hal_.display->off();

  const uint32_t now = hal_.clock.uptimeMs();
  lastMeasureMs_ = now;
  lastSaveMs_ = now;
  measureDue_ = true;
  nextTelegramMs_ = now + 5000;  // first telegram shortly after boot
  HKV_LOG("HKV id=%08lu units=%lu", static_cast<unsigned long>(settings_.meterId),
          static_cast<unsigned long>(ledger_.currentUnits()));
}

bool HkvApp::localTime(DateTime& out) const {
  if (!hal_.clock.isTimeValid()) return false;
  out = toLocal(hal_.clock.nowUtc(), settings_.utcOffsetMin, settings_.euDst != 0);
  return true;
}

void HkvApp::poll() {
  const uint32_t now = hal_.clock.uptimeMs();

  // Clock just became valid (NTP, browser): measure at once so time-based state
  // (error flag, due date, summer mode) is correct immediately.
  const bool timeValid = hal_.clock.isTimeValid();
  if (timeValid && !lastTimeValid_) measureDue_ = true;
  lastTimeValid_ = timeValid;

  if (measureDue_ || reached(now, nextMeasureMs_)) {
    measureDue_ = false;
    measure();
    nextMeasureMs_ = hal_.clock.uptimeMs() + settings_.measureIntervalS * 1000u;
  }
  if (telegramDue_ || reached(now, nextTelegramMs_)) {
    telegramDue_ = false;
    sendTelegram();
    nextTelegramMs_ = hal_.clock.uptimeMs() + settings_.telegramIntervalS * 1000u;
  }
  if (dirty_) saveData(false);

  if (liveActive() && hal_.clock.uptimeMs() - lastLiveMs_ >= kLiveIntervalMs) liveMeasure();

  if (displayOn_) {
    if (reached(now, displayOffAtMs_)) {
      displayOn_ = false;
      if (hal_.display) hal_.display->off();
    } else if (now - lastDisplayRefreshMs_ >= kDisplayRefreshMs) {
      refreshDisplay();
    }
  }
}

uint32_t HkvApp::msUntilNextEvent() const {
  if (measureDue_ || telegramDue_) return 0;
  const uint32_t now = hal_.clock.uptimeMs();
  auto left = [now](uint32_t deadline) -> uint32_t {
    const int32_t d = static_cast<int32_t>(deadline - now);
    return d > 0 ? static_cast<uint32_t>(d) : 0u;
  };
  uint32_t ms = left(nextMeasureMs_);
  const uint32_t t = left(nextTelegramMs_);
  if (t < ms) ms = t;
  if (displayOn_) {
    const uint32_t r = kDisplayRefreshMs;
    if (r < ms) ms = r;
  }
  if (liveActive()) {
    const uint32_t l = left(lastLiveMs_ + kLiveIntervalMs);
    if (l < ms) ms = l;
  }
  return ms;
}

void HkvApp::measure() {
  const uint32_t now = hal_.clock.uptimeMs();
  float dtS = static_cast<float>(now - lastMeasureMs_) / 1000.0f;
  lastMeasureMs_ = now;
  // Protect against absurd gaps (e.g. after a debugger halt): at most 2 intervals + 60 s.
  const float maxDt = static_cast<float>(settings_.measureIntervalS) * 2.0f + 60.0f;
  if (dtS > maxDt) dtS = maxDt;

  snap_.radiatorRaw = hal_.radiator.read();
  snap_.roomRaw = hal_.room.read();
  snap_.battery = hal_.battery.read();

  DateTime local;
  const bool timeValid = localTime(local);
  const bool summer = timeValid && isSummer(settings_, local.month);

  // Rectangle rule: the rate at the end of the interval is applied to the whole interval.
  snap_.alloc = evaluate(settings_, snap_.radiatorRaw, snap_.roomRaw, summer);
  ledger_.accumulate(snap_.alloc.unitsPerHour, dtS);

  uint32_t err = 0;
  if (!snap_.radiatorRaw.valid) err |= kErrRadiatorSensor;
  if (!snap_.roomRaw.valid) err |= kErrRoomSensor;
  if (snap_.battery.valid && !snap_.battery.charging && snap_.battery.millivolts < settings_.batteryLowMv)
    err |= kErrBatteryLow;
  if (!timeValid) err |= kErrTimeNotSet;
  err |= data_.errorHistory & kErrStorage;
  snap_.errors = err;
  data_.errorHistory |= err & ~kErrTimeNotSet;

  if (timeValid && ledger_.updateCalendar(local, settings_)) saveData(true);

  if (ledger_.currentUnits() != savedUnits_) dirty_ = true;

  snap_.valid = true;
  snap_.measurements++;
  data_.lifetimeMeasurements++;
  snap_.uptimeMs = now;

  HKV_LOG("meas rad=%.2f room=%.2f dT=%.2f rate=%.3f/h state=%s units=%lu bat=%umV err=0x%02lx",
          static_cast<double>(snap_.alloc.radiatorC), static_cast<double>(snap_.alloc.roomC),
          static_cast<double>(snap_.alloc.deltaK), static_cast<double>(snap_.alloc.unitsPerHour),
          countStateName(snap_.alloc.state), static_cast<unsigned long>(ledger_.currentUnits()),
          static_cast<unsigned>(snap_.battery.millivolts), static_cast<unsigned long>(snap_.errors));

  if (displayOn_) refreshDisplay();
}

bool HkvApp::liveActive() const {
  const bool kept = static_cast<int32_t>(liveUntilMs_ - hal_.clock.uptimeMs()) > 0;
  return kept || (displayOn_ && page_ == Page::Temperatures);
}

void HkvApp::keepLiveAlive(uint32_t keepMs) {
  const bool wasActive = liveActive();
  liveUntilMs_ = hal_.clock.uptimeMs() + keepMs;
  if (!wasActive || !live_.valid) liveMeasure();  // first value right away
}

void HkvApp::liveMeasure() {
  lastLiveMs_ = hal_.clock.uptimeMs();
  live_.radiatorRaw = hal_.radiator.read();
  live_.roomRaw = hal_.room.read();
  DateTime local;
  const bool summer = localTime(local) && isSummer(settings_, local.month);
  live_.alloc = evaluate(settings_, live_.radiatorRaw, live_.roomRaw, summer);
  live_.uptimeMs = lastLiveMs_;
  live_.valid = true;
  if (displayOn_) refreshDisplay();
}

uint8_t HkvApp::omsStatus() const {
  uint8_t st = 0;
  const uint32_t e = snap_.errors;
  if (e & (kErrRadiatorSensor | kErrRoomSensor)) st |= oms::kStatusTemporaryError | oms::kStatusErrorAny;
  if (e & kErrStorage) st |= oms::kStatusPermanentError | oms::kStatusErrorAny;
  if (e & kErrBatteryLow) st |= oms::kStatusPowerLow;
  return st;
}

void HkvApp::sendTelegram() {
  oms::HcaTelegram t;
  t.manufacturer = settings_.manufacturer;
  t.meterId = settings_.meterId;
  t.version = settings_.deviceVersion;
  t.accessNo = ++data_.accessNo;
  t.status = omsStatus();
  t.security = settings_.omsSecurity;
  t.aesKey = settings_.aesKey;
  t.timeValid = localTime(t.now);
  t.currentUnits = ledger_.currentUnits();
  t.dueDateValid = data_.dueDateYear != 0;
  t.dueDate.year = data_.dueDateYear;
  t.dueDate.month = data_.dueDateMonth;
  t.dueDate.day = data_.dueDateDay;
  t.dueDateUnits = data_.dueDateUnits;
  t.radiatorValid = snap_.radiatorRaw.valid;
  t.radiatorC = snap_.alloc.radiatorC;
  t.roomValid = snap_.roomRaw.valid;
  t.roomC = snap_.alloc.roomC;

  telegramLen_ = oms::buildHcaTelegram(t, hal_.cipher, telegram_, sizeof(telegram_));
  if (telegramLen_ == 0) {
    HKV_LOG("telegram: build failed (security=%u)", static_cast<unsigned>(t.security));
    return;
  }
  if (hal_.sink) hal_.sink->send(telegram_, telegramLen_);
}

void HkvApp::saveData(bool force) {
  if (storageLocked_) return;
  const uint32_t now = hal_.clock.uptimeMs();
  const bool unitsChanged = ledger_.currentUnits() != savedUnits_;
  if (!force) {
    const bool minElapsed = now - lastSaveMs_ >= kSaveMinIntervalMs;
    const bool maxElapsed = now - lastSaveMs_ >= kSaveMaxIntervalMs;
    if (!(unitsChanged && minElapsed) && !maxElapsed) return;
  }
  if (saveObject(hal_.storage, kKeyData, kDataVersion, data_)) {
    data_.errorHistory &= ~kErrStorage;
    savedUnits_ = ledger_.currentUnits();
    dirty_ = false;
  } else {
    data_.errorHistory |= kErrStorage;
    HKV_LOG("storage: write failed");
  }
  lastSaveMs_ = now;
}

const char* HkvApp::applySettings(const Settings& s) {
  const char* err = validate(s);
  if (err) return err;
  if (storageLocked_) return "storage locked (factory reset pending)";
  const bool dueChanged = s.dueMonth != settings_.dueMonth || s.dueDay != settings_.dueDay;
  settings_ = s;
  if (hal_.display) hal_.display->configure(settings_.displayInvert != 0, settings_.displayFlip != 0);
  hal_.radiator.setEmissivity(settings_.emissivity);
  if (!saveObject(hal_.storage, kKeySettings, kSettingsVersion, settings_)) return "storage write failed";
  if (dueChanged) {
    data_.lastPeriodYear = -1;  // re-anchor the billing period, no spurious rollover
    saveData(true);
  }
  nextMeasureMs_ = hal_.clock.uptimeMs() + settings_.measureIntervalS * 1000u;
  nextTelegramMs_ = hal_.clock.uptimeMs() + settings_.telegramIntervalS * 1000u;
  HKV_LOG("settings saved");
  return nullptr;
}

void HkvApp::setTime(uint32_t utc) {
  hal_.clock.setUtc(utc);
  HKV_LOG("time set: %lu", static_cast<unsigned long>(utc));
  measureDue_ = true;
}

void HkvApp::resetLifetime() {
  data_.lifetimeMilliUnits = 0;
  data_.lifetimeMeasurements = 0;
  saveData(true);
  HKV_LOG("lifetime counter reset to 0");
}

void HkvApp::resetCurrentPeriod() {
  ledger_.resetCurrent();
  saveData(true);
  HKV_LOG("current period reset to 0");
}

const char* HkvApp::setCounterOffset(int64_t milliUnits) {
  if (milliUnits < 0 || milliUnits > 16000000000LL) return "offset out of range";
  DateTime local;
  const bool timeValid = localTime(local);
  ledger_.setOffset(milliUnits, timeValid ? &local : nullptr);
  saveData(true);
  HKV_LOG("counter offset set to %ld.%03ld units", static_cast<long>(milliUnits / 1000),
          static_cast<long>(milliUnits % 1000));
  return nullptr;
}

void HkvApp::eraseStoredData() {
  storageLocked_ = true;
  hal_.storage.erase(kKeySettings);
  hal_.storage.erase(kKeyData);
  HKV_LOG("factory reset: settings and meter data erased");
}

bool HkvApp::deleteMonthly(uint16_t year, uint8_t month) {
  if (!ledger_.deleteMonthly(year, month)) return false;
  saveData(true);
  HKV_LOG("monthly value %04u-%02u deleted", year, month);
  return true;
}

void HkvApp::buttonShortPress() {
  if (!displayOn_) {
    // A low battery is announced first, the next press continues with the normal cycle.
    showPage((snap_.errors & kErrBatteryLow) ? Page::BatteryLow : Page::Consumption);
    return;
  }
  const uint8_t count = static_cast<uint8_t>(Page::Count);
  const uint8_t cur = static_cast<uint8_t>(page_);
  showPage(cur >= count ? Page::Consumption : static_cast<Page>((cur + 1) % count));
}

void HkvApp::showPage(Page page) {
  const bool enteringTemps = page == Page::Temperatures && !(displayOn_ && page_ == Page::Temperatures);
  page_ = page;
  if (enteringTemps) lastLiveMs_ = hal_.clock.uptimeMs() - kLiveIntervalMs;  // live value at once
  displayOn_ = true;
  displayOffAtMs_ = hal_.clock.uptimeMs() + settings_.displayTimeoutS * 1000u;
  refreshDisplay();
}

void HkvApp::refreshDisplay() {
  lastDisplayRefreshMs_ = hal_.clock.uptimeMs();
  if (!hal_.display) return;
  DisplayModel m;
  m.page = page_;
  m.uptimeMs = lastDisplayRefreshMs_;
  m.meterId = settings_.meterId;
  m.currentMilliUnits = data_.currentMilliUnits;
  m.timeValid = localTime(m.local);
  if (m.timeValid) {
    m.periodStartValid = true;
    m.periodStart = Ledger::periodStart(m.local, settings_.dueMonth, settings_.dueDay);
  }
  m.dueDateValid = data_.dueDateYear != 0;
  m.dueDate.year = data_.dueDateYear;
  m.dueDate.month = data_.dueDateMonth;
  m.dueDate.day = data_.dueDateDay;
  m.dueMilliUnits = data_.dueDateMilliUnits;
  // Live values (if fresh) for the temperatures, the unit state stays from the real measurement.
  const bool useLive = liveActive() && live_.valid;
  const TempReading& rad = useLive ? live_.radiatorRaw : snap_.radiatorRaw;
  const TempReading& room = useLive ? live_.roomRaw : snap_.roomRaw;
  m.alloc = snap_.alloc;
  if (useLive) {
    m.alloc.radiatorC = live_.alloc.radiatorC;
    m.alloc.roomC = live_.alloc.roomC;
    m.alloc.deltaK = live_.alloc.deltaK;
  }
  m.live = useLive;
  m.radiatorValid = rad.valid;
  m.roomValid = room.valid;
  m.humidityPct = room.humidityPct;
  m.pressureHpa = room.pressureHpa;
  m.batteryValid = snap_.battery.valid;
  m.batteryMv = snap_.battery.millivolts;
  m.batteryPercent = snap_.battery.percent;
  m.charging = snap_.battery.charging;
  m.batteryLow = (snap_.errors & kErrBatteryLow) != 0;
  m.errors = snap_.errors;
  m.net = net_;
  hal_.display->show(m);
}

}  // namespace hkv

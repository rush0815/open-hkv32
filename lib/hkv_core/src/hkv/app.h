#pragma once
// HkvApp: the complete device logic, independent of the platform.
// The port provides the HAL objects and calls poll() from its main loop / RTOS task.
// msUntilNextEvent() tells the port how long it may sleep.

#include <stddef.h>
#include <stdint.h>

#include "hkv/allocator.h"
#include "hkv/calibration.h"
#include "hkv/display_model.h"
#include "hkv/hal.h"
#include "hkv/ledger.h"
#include "hkv/oms.h"
#include "hkv/settings.h"

namespace hkv {

struct Hal {
  IClock& clock;
  ITemperatureSensor& radiator;
  ITemperatureSensor& room;
  IBattery& battery;
  IStorage& storage;
  IDisplay* display;      // optional
  IBlockCipher* cipher;   // required for OMS mode 5
  ITelegramSink* sink;    // optional
  // Optional temporary reference room sensor for the calibration run (plugged in only
  // for calibration). May be nullptr if the port has none.
  ITemperatureSensor* reference = nullptr;
  // Optional working memory: the calibration run stores its samples there (10 bytes per
  // 30 s, ~2.4 KB for 2 h) for the outlier check, and releases it when the run has ended.
  IMemory* memory = nullptr;
};

// Display-only readings ("live mode"): taken every 2 s while someone is watching.
// They never feed the unit integration, which keeps using Snapshot / measureIntervalS.
struct LiveValues {
  bool valid = false;
  TempReading radiatorRaw;
  TempReading roomRaw;
  TempReading referenceRaw;  // only while a calibration run is in progress
  AllocResult alloc;
  uint32_t uptimeMs = 0;
};

struct Snapshot {
  bool valid = false;
  TempReading radiatorRaw;
  TempReading roomRaw;
  BatteryStatus battery;
  AllocResult alloc;
  uint32_t errors = 0;
  uint32_t measurements = 0;
  uint32_t uptimeMs = 0;
};

enum class CalibState : uint8_t { Idle = 0, Running = 1, Done = 2, Aborted = 3 };
enum class CalibAbort : uint8_t { None = 0, User = 1, ReferenceLost = 2, SensorLost = 3 };

// Calibration run of the room sensor against the reference sensor (see calibration.h).
struct CalibStatus {
  CalibState state = CalibState::Idle;
  CalibAbort abort = CalibAbort::None;
  uint32_t durationS = 0;
  uint32_t elapsedS = 0;
  uint32_t warmupS = 0;       // first part of the run: filters settle, not used for c / tau
  uint32_t samplesRead = 0;   // all samples incl. warm-up
  uint8_t missedInRow = 0;
  TempReading lastReference;  // last readings of the run (radiator incl. offset)
  float lastRadiatorC = NAN;
  float lastRoomRawC = NAN;
  CalibResult result;         // interim while running, final (and applied) when finished
  bool applied = false;       // result written to the settings
};

class HkvApp {
 public:
  explicit HkvApp(const Hal& hal) : hal_(hal), ledger_(data_) {}

  void begin(uint32_t defaultMeterId);
  void poll();
  uint32_t msUntilNextEvent() const;

  // --- user interface ---
  void buttonShortPress();
  void showPage(Page page);  // also switches the display on
  bool displayActive() const { return displayOn_; }
  Page page() const { return page_; }
  void setNetInfo(const NetInfo& info) { net_ = info; }

  // --- live mode (display only) ---
  // Keeps live mode on for keepMs (web UI polls this). Measures at once if it was off.
  void keepLiveAlive(uint32_t keepMs = 6000);
  void stopLive() { liveUntilMs_ = hal_.clock.uptimeMs(); }
  // Active while kept alive or while the display shows the temperature page.
  bool liveActive() const;
  const LiveValues& live() const { return live_; }
  // Radiator temperature (raw) lagged like the room sensor - used for the coupling correction.
  float radiatorLagC() const { return radiatorLagC_; }
  static constexpr uint32_t kLiveIntervalMs = 2000;

  // --- configuration / service (web portal, serial, NFC, ...) ---
  const Settings& settings() const { return settings_; }
  const char* applySettings(const Settings& s);  // nullptr = ok, otherwise error text
  void setTime(uint32_t utc);
  // --- meter data service (device exchange, corrections) ---
  void resetCurrentPeriod();
  void resetLifetime();  // total counter "since commissioning" back to 0
  const char* setCounterOffset(int64_t milliUnits);  // nullptr = ok
  bool deleteMonthly(uint16_t year, uint8_t month);
  // Factory reset of the core: erases settings and meter data from storage and blocks
  // further writes, so nothing is written back before the port restarts the device.
  void eraseStoredData();
  void requestMeasurement() { measureDue_ = true; }

  // --- calibration run with a temporary reference room sensor ---
  static constexpr uint32_t kCalibSampleMs = 30000;
  static constexpr uint32_t kCalibMinS = 60u, kCalibMaxS = 12u * 3600u;
  static constexpr uint8_t kCalibMaxMissed = 10;  // ~5 min without valid readings -> abort
  bool hasReferencePort() const { return hal_.reference != nullptr; }
  // Probes the reference sensor (hot plug). Fills r; false if none answers.
  bool readReference(TempReading& r);
  const char* referenceName() const { return hal_.reference ? hal_.reference->name() : ""; }
  // A meter counts only after a calibration run. The run is always started by hand and
  // ends by itself after durationS; the result is computed and applied at once, then the
  // meter counts. Ended early (by hand or because a sensor is lost), the values are
  // computed from the data collected so far and applied just the same.
  const char* startCalibration(uint32_t durationS);  // nullptr = started, otherwise error text
  void stopCalibration();  // ends the run early (development version)
  bool calibrated() const { return settings_.calibrated != 0; }
  bool counting() const { return calibrated() && calib_.state != CalibState::Running; }
  const CalibStatus& calibration() const { return calib_; }
  void requestTelegram() { telegramDue_ = true; }

  // --- state ---
  const MeterData& meterData() const { return data_; }
  const Snapshot& snapshot() const { return snap_; }
  uint32_t currentUnits() const { return ledger_.currentUnits(); }
  bool localTime(DateTime& out) const;
  const uint8_t* lastTelegram(size_t& len) const {
    len = telegramLen_;
    return telegram_;
  }
  bool monthlyValue(int ageIndex, MonthlyValue& out) const { return ledger_.monthlyValue(ageIndex, out); }

  static constexpr uint16_t kSettingsVersion = 6;
  static constexpr uint16_t kDataVersion = 5;

 private:
  void measure();
  void liveMeasure();
  void sendTelegram();
  void saveData(bool force);
  void refreshDisplay();
  uint8_t omsStatus() const;

  Hal hal_;
  Settings settings_{};
  MeterData data_{};
  Ledger ledger_;
  Snapshot snap_;

  uint32_t lastMeasureMs_ = 0;
  uint32_t nextMeasureMs_ = 0;
  uint32_t nextTelegramMs_ = 0;
  bool measureDue_ = true;
  bool lastTimeValid_ = false;
  bool timeGapOpen_ = false;  // booted without valid time, waiting for NTP/browser
  bool telegramDue_ = false;

  uint32_t lastSaveMs_ = 0;
  uint32_t savedUnits_ = 0;
  bool dirty_ = false;
  bool storageLocked_ = false;

  bool displayOn_ = false;
  uint32_t displayOffAtMs_ = 0;
  uint32_t lastDisplayRefreshMs_ = 0;
  Page page_ = Page::Consumption;
  NetInfo net_;

  LiveValues live_;
  float radiatorLagC_ = NAN;
  uint32_t radiatorLagMs_ = 0;
  void updateRadiatorLag(const TempReading& rad);
  uint32_t liveUntilMs_ = 0;
  uint32_t lastLiveMs_ = 0;

  void calibrationSample();
  void finishCalibration(CalibState st, CalibAbort why);
  void applyCalibration();
  void gateCounting(AllocResult& a) const;
  CouplingCalibrator calibrator_;
  CalibStatus calib_;
  uint32_t calibStartMs_ = 0;
  uint32_t nextCalibMs_ = 0;
  uint32_t lastCalibSampleMs_ = 0;
  bool calibFirst_ = true;
  float calibStartOffset_ = 0.0f, calibStartCoupling_ = 0.0f, calibStartTau_ = 0.0f;
  CalibSample* calibBuf_ = nullptr;  // from hal_.memory, only while a run is in progress
  size_t calibCap_ = 0, calibLen_ = 0;
  void releaseCalibBuffer();

  uint8_t telegram_[oms::kMaxFrame] = {0};
  size_t telegramLen_ = 0;
};

}  // namespace hkv

#pragma once
// Battery voltage of the TinyPICO (LiPo on the BAT pad, divider on GPIO35).
// On the CC1312 this becomes the AON battery monitor (BATMON) + a primary-cell curve.

#include <Arduino.h>

#include "board_tinypico.h"
#include "hkv/hal.h"

namespace port {

class TinyPicoBattery : public hkv::IBattery {
 public:
  bool begin() override {
    pinMode(board::kChargeState, INPUT);
    analogReadMilliVolts(board::kBatteryAdc);  // configures the pin as ADC channel first
    analogSetPinAttenuation(board::kBatteryAdc, ADC_11db);
    return true;
  }

  hkv::BatteryStatus read() override {
    uint32_t sum = 0;
    constexpr int kSamples = 8;
    for (int i = 0; i < kSamples; ++i) sum += analogReadMilliVolts(board::kBatteryAdc);
    hkv::BatteryStatus b;
    b.millivolts = static_cast<uint16_t>(static_cast<float>(sum) / kSamples * board::kBatteryDivider);
    b.valid = b.millivolts > 2500;  // below that: no battery / floating input
    if (board::kUsbSense >= 0) {
      const bool usb = analogReadMilliVolts(board::kUsbSense) > 1500;
      b.charging = usb && digitalRead(board::kChargeState) == LOW;
    } else {
      b.charging = false;  // not detectable without USB sense (see board_tinypico.h)
    }
    b.percent = liPoPercent(b.millivolts);
    return b;
  }

  static uint8_t liPoPercent(uint16_t mv) {
    // Rough open-circuit curve of a single LiPo cell.
    static const uint16_t kMv[] = {3300, 3500, 3600, 3700, 3750, 3800, 3900, 4000, 4100, 4200};
    static const uint8_t kPct[] = {0, 5, 10, 25, 40, 50, 65, 80, 90, 100};
    if (mv <= kMv[0]) return 0;
    for (size_t i = 1; i < sizeof(kMv) / sizeof(kMv[0]); ++i) {
      if (mv <= kMv[i]) {
        return static_cast<uint8_t>(kPct[i - 1] + (kPct[i] - kPct[i - 1]) * (mv - kMv[i - 1]) / (kMv[i] - kMv[i - 1]));
      }
    }
    return 100;
  }
};

}  // namespace port

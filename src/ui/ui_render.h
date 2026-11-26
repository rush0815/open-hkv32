#pragma once
// Screen layouts for the 160x80 landscape TFT (ST7735S).
// Only uses the Adafruit_GFX drawing API, so the same code runs on the device
// (rendered into a GFXcanvas16 and pushed in one go) and in the PC preview
// tool (tools/ui_preview).

#include <Adafruit_GFX.h>

#include "hkv/display_model.h"

namespace ui {

constexpr int kWidth = 160;
constexpr int kHeight = 80;

// QR code modules (filled by the port, e.g. with esp_qrcode).
struct QrMatrix {
  static constexpr int kMax = 41;  // up to version 6
  uint8_t size = 0;
  uint8_t bits[(kMax * kMax + 7) / 8] = {0};
  bool get(int x, int y) const {
    const int i = y * size + x;
    return (bits[i >> 3] >> (i & 7)) & 1;
  }
  void set(int x, int y, bool v) {
    const int i = y * size + x;
    if (v) bits[i >> 3] |= static_cast<uint8_t>(1 << (i & 7));
    else bits[i >> 3] &= static_cast<uint8_t>(~(1 << (i & 7)));
  }
};

void render(Adafruit_GFX& g, const hkv::DisplayModel& m, const QrMatrix* qr);

}  // namespace ui

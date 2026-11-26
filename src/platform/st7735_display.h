#pragma once
// ST7735S 80x160 TFT implementation of hkv::IDisplay (portrait).
// Every frame is drawn into an off-screen canvas (ui::render) and pushed in one go,
// so there is no flicker. The TFT is by far the largest consumer of the prototype;
// the final CC1312 device should use a segment/memory LCD, e-paper or no display.

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

#include "hkv/display_model.h"
#include "hkv/hal.h"
#include "ui/ui_render.h"

namespace port {

// Adafruit_ST7735 always initialises at its fixed 32 MHz. This subclass sends the
// (identical) init sequence a second time at a configurable, in-spec SPI clock.
class SlowSt7735 : public Adafruit_ST7735 {
 public:
  using Adafruit_ST7735::Adafruit_ST7735;
  void initMini160x80(uint8_t variant, uint32_t spiHz);
};

class St7735Display : public hkv::IDisplay {
 public:
  St7735Display();
  bool begin() override;
  void show(const hkv::DisplayModel& m) override;
  void off() override;
  void configure(bool invert, bool flip) override;

  void setQr(const ui::QrMatrix* qr) { qr_ = qr; }

 private:
  SlowSt7735 tft_;
  void selfTest();
  GFXcanvas16* canvas_ = nullptr;
  const ui::QrMatrix* qr_ = nullptr;
  bool on_ = false;
  bool invert_ = true;
  bool flip_ = false;
};

}  // namespace port

#include "platform/st7735_display.h"

#include <SPI.h>
#include <driver/gpio.h>

#include "board_tinypico.h"
#include "hkv/log.h"
#include "hkv_features.h"

namespace port {

// BLK: HIGH = backlight on, LOW = off. While off the level is latched (gpio_hold), so
// the pin cannot float during light sleep - many modules pull BLK up to VCC (= on).
static void setBacklight(bool on) {
  if (!board::kTftBacklightSwitched) return;
  const gpio_num_t pin = static_cast<gpio_num_t>(board::kTftBacklight);
  gpio_hold_dis(pin);
  digitalWrite(board::kTftBacklight, on ? HIGH : LOW);
  if (!on) gpio_hold_en(pin);
}

// Copies of the init command lists (ST7735R, mini 160x80) from Adafruit_ST7735.cpp,
// Adafruit ST7735 and ST7789 Library, Copyright (c) Adafruit Industries, MIT license.
// clang-format off
static const uint8_t kInit1[] = {
  15,
  ST77XX_SWRESET, ST_CMD_DELAY, 150,
  ST77XX_SLPOUT,  ST_CMD_DELAY, 255,
  ST7735_FRMCTR1, 3, 0x01, 0x2C, 0x2D,
  ST7735_FRMCTR2, 3, 0x01, 0x2C, 0x2D,
  ST7735_FRMCTR3, 6, 0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D,
  ST7735_INVCTR,  1, 0x07,
  ST7735_PWCTR1,  3, 0xA2, 0x02, 0x84,
  ST7735_PWCTR2,  1, 0xC5,
  ST7735_PWCTR3,  2, 0x0A, 0x00,
  ST7735_PWCTR4,  2, 0x8A, 0x2A,
  ST7735_PWCTR5,  2, 0x8A, 0xEE,
  ST7735_VMCTR1,  1, 0x0E,
  ST77XX_INVOFF,  0,
  ST77XX_MADCTL,  1, 0xC8,
  ST77XX_COLMOD,  1, 0x05 };
static const uint8_t kInit2Mini[] = {
  2,
  ST77XX_CASET, 4, 0x00, 0x00, 0x00, 0x4F,
  ST77XX_RASET, 4, 0x00, 0x00, 0x00, 0x9F };
static const uint8_t kInit2MiniPlugin[] = {
  3,
  ST77XX_INVON, 0,
  ST77XX_CASET, 4, 0x00, 0x00, 0x00, 0x4F,
  ST77XX_RASET, 4, 0x00, 0x00, 0x00, 0x9F };
static const uint8_t kInit3[] = {
  4,
  ST7735_GMCTRP1, 16, 0x02, 0x1c, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2d,
                      0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10,
  ST7735_GMCTRN1, 16, 0x03, 0x1d, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D,
                      0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10,
  ST77XX_NORON,  ST_CMD_DELAY, 10,
  ST77XX_DISPON, ST_CMD_DELAY, 100 };
// clang-format on

void SlowSt7735::initMini160x80(uint8_t variant, uint32_t spiHz) {
  initR(variant);        // sets geometry/offsets (commands sent at the library's 32 MHz)
  setSPISpeed(spiHz);    // ... then the whole sequence again at an in-spec clock
  displayInit(kInit1);
  displayInit(variant == INITR_MINI160x80_PLUGIN ? kInit2MiniPlugin : kInit2Mini);
  displayInit(kInit3);
  if (variant == INITR_MINI160x80) {
    uint8_t data = 0xC0;
    sendCommand(ST77XX_MADCTL, &data, 1);
  }
}

St7735Display::St7735Display() : tft_(board::kTftCs, board::kTftDc, board::kTftRst) {}

bool St7735Display::begin() {
  if (board::kTftBacklightSwitched) {
    gpio_hold_dis(static_cast<gpio_num_t>(board::kTftBacklight));
    pinMode(board::kTftBacklight, OUTPUT);
    setBacklight(false);  // dark until the first frame is drawn
  }
  SPI.begin(board::kTftSck, -1, board::kTftMosi, board::kTftCs);
  tft_.initMini160x80(board::kTftInitVariant, board::kTftSpiHz);
  tft_.setRotation(flip_ ? (board::kTftRotation + 2) % 4 : board::kTftRotation);
  tft_.invertDisplay(invert_);
  tft_.fillScreen(ST77XX_BLACK);
  canvas_ = new GFXcanvas16(ui::kWidth, ui::kHeight);
  if (!canvas_->getBuffer()) {  // 25 KB RAM
    delete canvas_;
    canvas_ = nullptr;
    HKV_LOG("display: frame buffer allocation FAILED (free heap %u)", static_cast<unsigned>(ESP.getFreeHeap()));
    return false;
  }
  HKV_LOG("display: ST7735 init ok (variant 0x%02X, %lu Hz, CS=%d DC=%d RST=%d BLK=%d), heap %u",
          board::kTftInitVariant, static_cast<unsigned long>(board::kTftSpiHz), board::kTftCs, board::kTftDc,
          board::kTftRst, board::kTftBacklight, static_cast<unsigned>(ESP.getFreeHeap()));
#if HKV_DISPLAY_SELFTEST
  selfTest();
#endif
  on_ = true;
  return true;
}

// Full-screen colours: easy to judge by eye whether data reaches the controller.
void St7735Display::selfTest() {
  HKV_LOG("display: self-test white/red/green/blue");
  setBacklight(true);
  tft_.fillScreen(ST77XX_WHITE);
  delay(2000);
  tft_.fillScreen(ST77XX_RED);
  delay(1000);
  tft_.fillScreen(ST77XX_GREEN);
  delay(1000);
  tft_.fillScreen(ST77XX_BLUE);
  delay(1000);
  tft_.fillScreen(ST77XX_BLACK);
}

void St7735Display::configure(bool invert, bool flip) {
  if (invert == invert_ && flip == flip_) return;
  invert_ = invert;
  flip_ = flip;
  tft_.invertDisplay(invert_);
  tft_.setRotation(flip_ ? (board::kTftRotation + 2) % 4 : board::kTftRotation);
  HKV_LOG("display: invert=%u flip=%u", invert_, flip_);
}

void St7735Display::off() {
  if (!on_) return;
  tft_.fillScreen(ST77XX_BLACK);
  tft_.enableDisplay(false);
  tft_.enableSleep(true);
  setBacklight(false);
  on_ = false;
  HKV_LOG("display: off");
}

void St7735Display::show(const hkv::DisplayModel& m) {
  if (!canvas_) {
    HKV_LOG("display: show() without frame buffer");
    return;
  }
  ui::render(*canvas_, m, qr_);
  if (!on_) {
    HKV_LOG("display: on (page %u), BLK -> HIGH", static_cast<unsigned>(m.page));
    tft_.enableSleep(false);
    delay(120);  // ST7735: wait after SLPOUT
    tft_.enableDisplay(true);
    on_ = true;
  }
  tft_.drawRGBBitmap(0, 0, canvas_->getBuffer(), ui::kWidth, ui::kHeight);
  setBacklight(true);  // only after the frame is complete -> no flash of old content
}

}  // namespace port

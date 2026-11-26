#pragma once
// Pin map for the Unexpected Maker TinyPICO (V1/V2/V3, ESP32-PICO).
// GPIO numbers (not header positions!). Source: board variant um_tinypico/pins_arduino.h
// and the Unexpected Maker TinyPICO helper library.
//
// Header, top view, USB on top:
//          left            right
//   GPIO23 MOSI  o      o  BAT
//   GPIO19 MISO  o      o  GND
//   GPIO18 SCK   o      o  5V
//   GPIO5  SS    o      o  3V3
//   GPIO22 SCL   o      o  GPIO4
//   GPIO21 SDA   o      o  GPIO14
//   GPIO32       o      o  GPIO15
//   GPIO33       o      o  GPIO27
//   RESET        o      o  GPIO26
//   GND          o      o  GPIO25

#include <stdint.h>

namespace board {

// I2C bus: BMP280 (room) and later MLX90614 (radiator, SMBus max. 100 kHz)
constexpr int kI2cSda = 21;
constexpr int kI2cScl = 22;
constexpr uint32_t kI2cHz = 100000;

// ST7735S 80x160 TFT on the hardware SPI pins
constexpr int kTftSck = 18;
constexpr int kTftMosi = 23;
constexpr int kTftCs = 5;
constexpr int kTftDc = 25;
constexpr int kTftRst = 26;
constexpr int kTftBacklight = 27;          // BLK input of the module: HIGH = on, LOW (GND) = off
constexpr bool kTftBacklightSwitched = true;
constexpr uint8_t kTftRotation = 1;        // 1 = landscape 160x80 (3 = turned by 180 deg, see web UI)
// INITR_MINI160x80 (0x04) or INITR_MINI160x80_PLUGIN (0x06, inverted colours)
constexpr uint8_t kTftInitVariant = 0x04;
// Colour inversion and 180 degree rotation are settings (web UI: Service -> Display),
// because otherwise identical modules differ here.
// SPI clock. The ST7735 datasheet allows ~15 MHz for writes; the Adafruit default of
// 32 MHz is out of spec and unreliable over jumper wires.
constexpr uint32_t kTftSpiHz = 8000000;

// Push button to GND (internal pull-up). GPIO4 is an RTC GPIO -> usable as
// deep-sleep wake source later.
constexpr int kButton = 4;

// WiFi button to GND (internal pull-up): switches WiFi on for the configured wake time,
// also out of light sleep. GPIO14 = right header, 6th pin.
constexpr int kWifiButton = 14;

// On-board, not on the header
constexpr int kBatteryAdc = 35;            // divider 442k / 160k
constexpr float kBatteryDivider = (442.0f + 160.0f) / 160.0f;
constexpr int kChargeState = 34;           // LOW = charging - BUT also LOW without USB (see below)
// The charger's STAT pull-up is powered from USB, so without USB GPIO34 reads LOW too
// and "charging" cannot be told apart from "no USB". Optional USB sense: header pin
// "5V" -> 100k -> GPIO33 -> 100k -> GND (max. ~2.5 V at the pin). -1 = not fitted:
// then the charging state is not reported at all.
constexpr int kUsbSense = -1;
constexpr int kDotstarPower = 13;          // HIGH = LED power off
constexpr int kDotstarData = 2;
constexpr int kDotstarClock = 12;

}  // namespace board

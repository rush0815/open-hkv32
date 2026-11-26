// HKV - open source heat cost allocator, ESP32 (TinyPICO) prototype port.
//
// This file only wires platform drivers to the portable core (lib/hkv_core).
// A CC1312 port provides its own main with the same structure.

#include <Arduino.h>
#include <Wire.h>

#include "board_tinypico.h"
#include "hkv_features.h"
#include "hkv/aes128.h"
#include "hkv/app.h"
#include "hkv/log.h"
#include "net/net_manager.h"
#include "platform/bmp280_room_sensor.h"
#include "platform/button.h"
#include "platform/esp32_basics.h"
#include "platform/power.h"
#include "platform/serial_telegram_sink.h"
#include "platform/tinypico_battery.h"

#if HKV_RADIATOR_SENSOR == 0
#include "hkv/sim/emulated_radiator.h"
#endif
#include "platform/mlx90614_sensor.h"

#if HKV_ENABLE_DISPLAY
#include "platform/st7735_display.h"
#endif

namespace {

port::Esp32Clock g_clock;
port::Esp32Storage g_storage;
port::Esp32Random g_random;
port::TinyPicoBattery g_battery;
port::Bmp280RoomSensor g_room(Wire);

#if HKV_RADIATOR_SENSOR == 0
hkv::EmulatedRadiatorConfig makeEmulationConfig() {
  hkv::EmulatedRadiatorConfig c;
  c.timeScale = HKV_EMULATION_TIME_SCALE;
  return c;
}
hkv::EmulatedRadiatorSensor g_radiator(g_clock, g_random, makeEmulationConfig());
#else
port::Mlx90614Sensor g_radiator(Wire);
#endif

#if HKV_ENABLE_DISPLAY
port::St7735Display g_display;
hkv::IDisplay* const g_displayPtr = &g_display;
#else
hkv::IDisplay* const g_displayPtr = nullptr;
#endif

hkv::SoftAes128 g_aes;
port::SerialTelegramSink g_sink;

hkv::HkvApp g_app(hkv::Hal{g_clock, g_radiator, g_room, g_battery, g_storage, g_displayPtr, &g_aes, &g_sink});
net::NetManager g_net(g_app, g_storage);
port::Button g_button(board::kButton, HKV_LONG_PRESS_MS);
port::Button g_wifiButton(board::kWifiButton, HKV_LONG_PRESS_MS);

uint32_t defaultMeterId() {
  // Derive a stable default id from the factory MAC; can be changed in the web UI.
  const uint64_t mac = ESP.getEfuseMac();
  return static_cast<uint32_t>((mac >> 16) ^ mac) % 100000000u;
}

void logToSerial(const char* line) {
  Serial.printf("[%8lu] %s\n", static_cast<unsigned long>(millis()), line);
}

uint32_t g_lastNetInfoMs = 0;

// ---- MLX90614 diagnostics (read only, does not affect counting) ----
port::Mlx90614Sensor g_mlxDiag(Wire);
#if HKV_MLX_DIAG
uint32_t g_lastMlxDiagMs = 0;
#endif

#if HKV_DISPLAY_SELFTEST
// Wiring check of the display lines before the SPI driver takes them over.
// Each pin is read with internal pull-up and pull-down: a free line follows the pull
// (1/0); stuck at 0 = short to GND, stuck at 1 = short to 3V3 or a strong pull-up on
// the module. Then each pin is driven and the others are checked for following it.
void displayPinTest() {
  struct P { int pin; const char* name; };
  const P pins[] = {{board::kTftSck, "SCK"}, {board::kTftMosi, "MOSI"}, {board::kTftCs, "CS"},
                    {board::kTftDc, "DC"},   {board::kTftRst, "RST"},  {board::kTftBacklight, "BLK"}};
  const int n = sizeof(pins) / sizeof(pins[0]);
  char line[160] = "pin test (pullup/pulldown):";
  for (int i = 0; i < n; ++i) {
    pinMode(pins[i].pin, INPUT_PULLUP);
    delay(2);
    const int up = digitalRead(pins[i].pin);
    pinMode(pins[i].pin, INPUT_PULLDOWN);
    delay(2);
    const int down = digitalRead(pins[i].pin);
    char b[24];
    snprintf(b, sizeof(b), " %s=%d/%d%s", pins[i].name, up, down, (up == 1 && down == 0) ? "" : "!");
    strncat(line, b, sizeof(line) - strlen(line) - 1);
  }
  HKV_LOG("%s", line);
  bool shorts = false;
  for (int i = 0; i < n; ++i) {
    for (int lvl = 0; lvl < 2; ++lvl) {
      for (int k = 0; k < n; ++k) pinMode(pins[k].pin, lvl ? INPUT_PULLDOWN : INPUT_PULLUP);
      pinMode(pins[i].pin, OUTPUT);
      digitalWrite(pins[i].pin, lvl ? HIGH : LOW);
      delay(2);
      for (int k = 0; k < n; ++k) {
        if (k == i) continue;
        if (digitalRead(pins[k].pin) == (lvl ? HIGH : LOW)) {
          HKV_LOG("pin test: %s follows %s (%s) -> short circuit?", pins[k].name, pins[i].name, lvl ? "HIGH" : "LOW");
          shorts = true;
        }
      }
    }
    pinMode(pins[i].pin, INPUT);
  }
  if (!shorts) HKV_LOG("pin test: no shorts between the display lines");
}
#endif

void i2cScan() {
  char line[96] = "i2c scan:";
  for (uint8_t a = 1; a < 127; ++a) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      char b[8];
      snprintf(b, sizeof(b), " 0x%02X", a);
      strncat(line, b, sizeof(line) - strlen(line) - 1);
    }
  }
  HKV_LOG("%s", line);
}

void mlxDiagInfo() {
  float e;
  uint16_t cfg, id[4];
  uint8_t adr;
  const bool okE = g_mlxDiag.emissivity(e);
  const bool okC = g_mlxDiag.config1(cfg);
  const bool okA = g_mlxDiag.smbusAddress(adr);
  const bool okI = g_mlxDiag.id(id);
  if (!okE && !okC && !okA && !okI) {
    HKV_LOG("mlx diag: no answer at 0x%02X", g_mlxDiag.address());
    return;
  }
  HKV_LOG("mlx diag: emissivity=%s%.3f eeprom-addr=0x%02X config1=0x%04X (dual-zone=%u, IIR=%u, FIR=%u, gain=%u)",
          okE ? "" : "?", okE ? static_cast<double>(e) : 0.0, okA ? adr : 0, okC ? cfg : 0, okC ? (cfg >> 6) & 1 : 0,
          okC ? cfg & 7 : 0, okC ? (cfg >> 8) & 7 : 0, okC ? (cfg >> 11) & 7 : 0);
  if (okI) HKV_LOG("mlx diag: id=%04X-%04X-%04X-%04X", id[0], id[1], id[2], id[3]);
}

void mlxDiagValues() {
  float ta, t1, t2;
  const bool a = g_mlxDiag.ambient(ta), o1 = g_mlxDiag.object1(t1), o2 = g_mlxDiag.object2(t2);
  HKV_LOG("mlx diag: ambient=%s object1=%s object2=%s", a ? String(ta, 2).c_str() : "err",
          o1 ? String(t1, 2).c_str() : "err", o2 ? String(t2, 2).c_str() : "n/a");
}

void updateNetInfo(bool force = false) {
  if (!force && millis() - g_lastNetInfoMs < 200) return;
  g_lastNetInfoMs = millis();
  hkv::NetInfo info;
  g_net.fillNetInfo(info);
  g_app.setNetInfo(info);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  hkv::log::setSink(logToSerial);
  HKV_LOG("HKV firmware " HKV_FIRMWARE_VERSION " (TinyPICO prototype), radiator sensor: %s", g_radiator.name());

  port::disableDotstar();
  g_button.begin();
  g_wifiButton.begin();
  Wire.begin(board::kI2cSda, board::kI2cScl, board::kI2cHz);
  if (!g_storage.begin()) HKV_LOG("NVS: open failed");

#if HKV_ENABLE_DISPLAY
  g_display.setQr(&g_net.qr());
#endif
#if HKV_DISPLAY_SELFTEST
  displayPinTest();
#endif
  i2cScan();
  mlxDiagInfo();
  mlxDiagValues();
  g_app.begin(defaultMeterId());
  g_net.begin();
  updateNetInfo(true);
}

void loop() {
  switch (g_button.poll()) {
    case port::Button::Event::Short:
      HKV_LOG("button: short press");
      g_app.buttonShortPress();
      break;
    case port::Button::Event::Long:
      HKV_LOG("button: long press");
      g_net.startPortal();
      break;
    case port::Button::Event::None:
      break;
  }
  if (g_wifiButton.poll() != port::Button::Event::None) {
    HKV_LOG("button: wifi");
    g_net.wake();
  }

  g_net.loop();
  updateNetInfo();
  if (g_net.takeAttention()) {  // connecting / failed / setup mode: show it on the display
    updateNetInfo(true);
    g_app.showPage(hkv::Page::Network);
  }
  g_app.poll();
#if HKV_MLX_DIAG
  if (millis() - g_lastMlxDiagMs >= 10000) {
    g_lastMlxDiagMs = millis();
    mlxDiagValues();
  }
#endif

  const bool canSleep = HKV_ENABLE_LIGHT_SLEEP && g_net.power().lightSleep && !g_net.busy() && !g_app.displayActive() &&
                        !g_button.active() && !g_wifiButton.active();
  if (canSleep) {
    const uint32_t ms = g_app.msUntilNextEvent();
    if (ms > 20) port::lightSleep(ms, board::kButton, board::kWifiButton);
  } else {
    delay(5);
  }
}

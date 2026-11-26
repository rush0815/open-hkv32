// Renders all screens with sample data into a PPM image (PC preview of the TFT UI).
// Build + run:  tools/ui_preview/run.sh
#include <Adafruit_GFX.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "ui/ui_render.h"

using hkv::DisplayModel;
using hkv::NetInfo;
using hkv::Page;

static DisplayModel base() {
  DisplayModel m;
  m.meterId = 12345678;
  m.uptimeMs = 1200;
  m.currentMilliUnits = 123456;
  m.timeValid = true;
  m.local.year = 2026; m.local.month = 11; m.local.day = 24;
  m.periodStartValid = true;
  m.periodStart.year = 2026; m.periodStart.month = 1; m.periodStart.day = 1;
  m.dueDateValid = true;
  m.dueDate.year = 2025; m.dueDate.month = 12; m.dueDate.day = 31;
  m.dueMilliUnits = 118700;
  m.alloc.state = hkv::CountState::Counting;
  m.alloc.radiatorC = 48.6f;
  m.alloc.roomC = 21.4f;
  m.alloc.deltaK = 27.2f;
  m.alloc.unitsPerHour = 0.45f;
  m.radiatorValid = m.roomValid = true;
  m.pressureHpa = 1013.2f;
  m.batteryValid = true;
  m.batteryMv = 3950;
  m.batteryPercent = 78;
  NetInfo& n = m.net;
  n.mode = NetInfo::Mode::Connected;
  strcpy(n.ssid, "FritzBox-7580");
  strcpy(n.ip, "192.168.178.142");
  n.channel = 11;
  n.rssi = -42;
  n.mqtt = NetInfo::Mqtt::Connected;
  n.mqttAgeS = 125;
  strcpy(n.mac, "AA:BB:CC:DD:EE:FF");
  strcpy(n.firmware, "0.3.0");
  strcpy(n.apSsid, "HKV-12345678");
  strcpy(n.apPass, "Ab3dEf7h");
  strcpy(n.apIp, "192.168.4.1");
  return m;
}

int main(int argc, char** argv) {
  const char* out = argc > 1 ? argv[1] : "preview.ppm";
  const char* qrFile = argc > 2 ? argv[2] : nullptr;

  ui::QrMatrix qr;
  if (qrFile) {
    FILE* f = fopen(qrFile, "r");
    char line[128];
    int y = 0;
    while (f && fgets(line, sizeof line, f)) {
      int len = static_cast<int>(strcspn(line, "\r\n"));
      if (len == 0) continue;
      qr.size = static_cast<uint8_t>(len);
      for (int x = 0; x < len; ++x) qr.set(x, y, line[x] == '1');
      ++y;
    }
    if (f) fclose(f);
  }

  std::vector<DisplayModel> screens;
  DisplayModel m = base();
  m.page = Page::Consumption; screens.push_back(m);
  m.page = Page::Temperatures; m.live = true; screens.push_back(m); m.live = false;
  m.page = Page::Network; screens.push_back(m);
  m.page = Page::Network; m.net.mode = NetInfo::Mode::AccessPoint; screens.push_back(m);
  m = base();
  m.page = Page::Network; m.net.mode = NetInfo::Mode::Connecting;
  m.net.attempt = 2; m.net.maxAttempts = 3; m.net.progressPct = 55; m.net.secondsLeft = 6; screens.push_back(m);
  m.net.mode = NetInfo::Mode::Failed; m.net.progressPct = 40; m.net.secondsLeft = 3; screens.push_back(m);
  m = base();
  m.page = Page::Network; m.net.mode = NetInfo::Mode::Off; screens.push_back(m);
  m.net.mode = NetInfo::Mode::Connected;
  m.page = Page::BatteryLow; m.batteryLow = true; m.batteryPercent = 12; screens.push_back(m);

  const int scale = 3, gap = 12, cols = 2;
  const int rows = (static_cast<int>(screens.size()) + cols - 1) / cols;
  const int W = cols * (ui::kWidth * scale + gap) + gap, H = rows * (ui::kHeight * scale + gap) + gap;
  std::vector<uint8_t> img(static_cast<size_t>(W) * H * 3, 40);

  GFXcanvas16 canvas(ui::kWidth, ui::kHeight);
  for (size_t i = 0; i < screens.size(); ++i) {
    ui::render(canvas, screens[i], &qr);
    const int ox = gap + static_cast<int>(i % cols) * (ui::kWidth * scale + gap);
    const int oy = gap + static_cast<int>(i / cols) * (ui::kHeight * scale + gap);
    for (int y = 0; y < ui::kHeight; ++y)
      for (int x = 0; x < ui::kWidth; ++x) {
        const uint16_t c = canvas.getPixel(x, y);
        const uint8_t r = static_cast<uint8_t>(((c >> 11) & 0x1F) * 255 / 31);
        const uint8_t g = static_cast<uint8_t>(((c >> 5) & 0x3F) * 255 / 63);
        const uint8_t b = static_cast<uint8_t>((c & 0x1F) * 255 / 31);
        for (int dy = 0; dy < scale; ++dy)
          for (int dx = 0; dx < scale; ++dx) {
            uint8_t* p = &img[((oy + y * scale + dy) * W + ox + x * scale + dx) * 3];
            p[0] = r; p[1] = g; p[2] = b;
          }
      }
  }
  FILE* f = fopen(out, "wb");
  fprintf(f, "P6\n%d %d\n255\n", W, H);
  fwrite(img.data(), 1, img.size(), f);
  fclose(f);
  printf("wrote %s (%dx%d)\n", out, W, H);
  return 0;
}

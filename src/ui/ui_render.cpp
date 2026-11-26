#include "ui/ui_render.h"

#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/TomThumb.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace ui {

using hkv::DisplayModel;
using hkv::NetInfo;
using hkv::Page;

namespace {

// ------------------------------------------------------------------ colours (RGB565)
constexpr uint16_t kBlack = 0x0000;
constexpr uint16_t kWhite = 0xFFFF;
constexpr uint16_t kGrey = 0xA514;      // secondary text
constexpr uint16_t kDarkGrey = 0x39E7;  // tracks, inactive
constexpr uint16_t kLine = 0x3CBC;      // separator lines (blue)
constexpr uint16_t kBlue = 0x451F;      // icons
constexpr uint16_t kGreen = 0x5EEB;
constexpr uint16_t kOrange = 0xFCA0;
constexpr uint16_t kRed = 0xE186;

constexpr float kPi = 3.14159265f;

// ------------------------------------------------------------------ text helpers

// UTF-8 -> CP437 for the classic 6x8 font (umlauts, degree sign).
void toCp437(const char* in, char* out, size_t cap) {
  size_t o = 0;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(in); *p && o + 1 < cap; ++p) {
    if (*p == 0xC3 && p[1]) {
      unsigned char c = 0;
      switch (p[1]) {
        case 0xA4: c = 0x84; break;  // ä
        case 0xB6: c = 0x94; break;  // ö
        case 0xBC: c = 0x81; break;  // ü
        case 0x84: c = 0x8E; break;  // Ä
        case 0x96: c = 0x99; break;  // Ö
        case 0x9C: c = 0x9A; break;  // Ü
        case 0x9F: c = 0xE1; break;  // ß
      }
      out[o++] = c ? static_cast<char>(c) : '?';
      ++p;
    } else if (*p == 0xC2 && p[1] == 0xB0) {
      out[o++] = static_cast<char>(0xF8);  // °
      ++p;
    } else {
      out[o++] = static_cast<char>(*p);
    }
  }
  out[o] = '\0';
}

class Painter {
 public:
  explicit Painter(Adafruit_GFX& g) : g_(g) {}
  Adafruit_GFX& g() { return g_; }

  // Classic 6x8 font, (x, y) = top left. Returns width in pixels.
  int text(int x, int y, uint16_t c, const char* s, bool bold = false) {
    char buf[48];
    toCp437(s, buf, sizeof(buf));
    g_.setFont(nullptr);
    g_.setTextSize(1);
    g_.setTextColor(c);
    g_.setCursor(x, y);
    g_.print(buf);
    if (bold) {
      g_.setCursor(x + 1, y);
      g_.print(buf);
    }
    return width(buf) + (bold ? 1 : 0);
  }
  static int width(const char* cp437) { return static_cast<int>(strlen(cp437)) * 6 - 1; }
  int textWidth(const char* s) {
    char buf[48];
    toCp437(s, buf, sizeof(buf));
    return width(buf);
  }
  void center(int y, uint16_t c, const char* s, bool bold = false) {
    text((kWidth - textWidth(s) - (bold ? 1 : 0)) / 2, y, c, s, bold);
  }
  void right(int xr, int y, uint16_t c, const char* s) { text(xr - textWidth(s), y, c, s); }

  // Narrow rendering for IP addresses: '.' and ':' only take 3 px.
  int compact(int x, int y, uint16_t c, const char* s) {
    g_.setFont(nullptr);
    g_.setTextSize(1);
    for (; *s; ++s) {
      if (*s == '.' || *s == ':') {
        g_.drawChar(x - 1, y, *s, c, c, 1);
        x += 3;
      } else {
        g_.drawChar(x, y, *s, c, c, 1);
        x += 6;
      }
    }
    return x;
  }

  // GFX free font, y = baseline. Returns width.
  int font(const GFXfont* f, int x, int baseline, uint16_t c, const char* s) {
    g_.setFont(f);
    g_.setTextSize(1);
    g_.setTextColor(c);
    g_.setCursor(x, baseline);
    g_.print(s);
    const int w = fontWidth(f, s);
    g_.setFont(nullptr);
    return w;
  }
  int fontWidth(const GFXfont* f, const char* s) {
    g_.setFont(f);
    int16_t x1, y1;
    uint16_t w, h;
    g_.getTextBounds(s, 0, 50, &x1, &y1, &w, &h);
    g_.setFont(nullptr);
    return static_cast<int>(w) + x1;
  }

  void hline(int y, uint16_t c = kLine, int x0 = 4, int x1 = kWidth - 4) { g_.drawFastHLine(x0, y, x1 - x0, c); }

  // Thick arc, angles in degrees, 0 = up, clockwise.
  void arc(int cx, int cy, int r, int thick, float a0, float a1, uint16_t c) {
    for (float a = a0; a <= a1; a += 1.2f) {
      const float s = sinf(a * kPi / 180.0f), co = cosf(a * kPi / 180.0f);
      for (int t = 0; t < thick; ++t) {
        const float rr = static_cast<float>(r - t);
        g_.drawPixel(cx + static_cast<int>(lroundf(rr * s)), cy - static_cast<int>(lroundf(rr * co)), c);
      }
    }
  }

 private:
  Adafruit_GFX& g_;
};

// ------------------------------------------------------------------ formatting

void fmtUnits(uint64_t milli, char* out, size_t cap) {
  const uint64_t tenths = milli / 100u;
  snprintf(out, cap, "%lu,%lu", static_cast<unsigned long>(tenths / 10u), static_cast<unsigned long>(tenths % 10u));
}

void fmt1(float v, char* out, size_t cap) {
  snprintf(out, cap, "%.1f", static_cast<double>(v));
  for (char* p = out; *p; ++p)
    if (*p == '.') *p = ',';
}


// Copies at most cap-1 characters (intentional truncation of long names).
void truncCopy(char* dst, size_t cap, const char* src) {
  size_t i = 0;
  for (; i + 1 < cap && src[i]; ++i) dst[i] = src[i];
  dst[i] = '\0';
}

// ------------------------------------------------------------------ icons

void iconHouse(Painter& p, int x, int y, uint16_t c) {
  Adafruit_GFX& g = p.g();
  g.fillTriangle(x + 5, y, x, y + 5, x + 10, y + 5, c);
  g.fillRect(x + 1, y + 5, 9, 5, c);
  g.fillRect(x + 4, y + 7, 3, 3, kBlack);  // door
}

// Wi-Fi fan, (x, y) = top left of a 13x10 box. level 0..3 = number of highlighted arcs.
void iconWifi(Painter& p, int x, int y, uint16_t on, int level) {
  const int cx = x + 6, cy = y + 9;
  p.g().fillCircle(cx, cy - 1, 1, level > 0 ? on : kDarkGrey);
  const int radii[3] = {4, 7, 10};
  for (int i = 0; i < 3; ++i) p.arc(cx, cy, radii[i], 2, -45.0f, 45.0f, level > i ? on : kDarkGrey);
}

void iconBattery(Painter& p, int x, int y, int w, int h, int pct, uint16_t c, bool valid) {
  Adafruit_GFX& g = p.g();
  g.drawRect(x, y, w - 2, h, valid ? kWhite : kGrey);
  g.fillRect(x + w - 2, y + h / 4, 2, h - 2 * (h / 4), valid ? kWhite : kGrey);
  if (!valid) return;
  const int inner = w - 6;
  int fill = inner * pct / 100;
  if (pct > 0 && fill < 1) fill = 1;
  g.fillRect(x + 2, y + 2, fill, h - 4, c);
}

void iconThermometer(Painter& p, int x, int y, uint16_t c) {
  Adafruit_GFX& g = p.g();
  g.drawRoundRect(x + 2, y, 7, 17, 3, kWhite);
  g.fillCircle(x + 5, y + 19, 5, kWhite);
  g.fillCircle(x + 5, y + 19, 4, c);
  g.fillRect(x + 4, y + 7, 3, 12, c);
}




void iconCloud(Painter& p, int x, int y, uint16_t c) {
  Adafruit_GFX& g = p.g();
  g.drawCircle(x + 4, y + 6, 3, c);
  g.drawCircle(x + 8, y + 4, 4, c);
  g.drawCircle(x + 12, y + 6, 3, c);
  g.fillRect(x + 4, y + 5, 9, 4, kBlack);
  g.drawFastHLine(x + 4, y + 9, 9, c);
}


void iconClock(Painter& p, int x, int y, uint16_t c) {
  Adafruit_GFX& g = p.g();
  g.drawCircle(x + 4, y + 4, 4, c);
  g.drawFastVLine(x + 4, y + 2, 3, c);
  g.drawFastHLine(x + 4, y + 4, 2, c);
}

void iconWarning(Painter& p, int cx, int y, int size, uint16_t c) {
  Adafruit_GFX& g = p.g();
  const int h = size * 87 / 100;
  g.fillTriangle(cx, y, cx - size / 2, y + h, cx + size / 2, y + h, c);
  g.fillTriangle(cx, y + 7, cx - size / 2 + 6, y + h - 3, cx + size / 2 - 6, y + h - 3, kBlack);
  g.fillRoundRect(cx - 2, y + h / 3, 4, h / 3, 1, c);
  g.fillRect(cx - 2, y + h - 9, 4, 4, c);
}


void iconDelta(Painter& p, int x, int y, uint16_t c, int size = 7) {
  Adafruit_GFX& g = p.g();
  g.drawTriangle(x + size / 2, y, x, y + size, x + size, y + size, c);
  g.drawLine(x + size / 2, y + 1, x + 1, y + size, c);
}

void statusDot(Painter& p, int x, int y, uint16_t c) { p.g().fillCircle(x + 3, y + 3, 3, c); }

int wifiLevel(int rssi) { return rssi >= -60 ? 3 : rssi >= -72 ? 2 : 1; }

// ------------------------------------------------------------------ header (landscape 160x80)
// Left: house + "HKV", centre: page title (or counting state), right: Wi-Fi + battery.
constexpr int kTop = 15;  // first content line below the header

void header(Painter& p, const DisplayModel& m, const char* title, uint16_t titleColour) {
  iconHouse(p, 2, 1, kWhite);
  p.text(15, 3, kWhite, "HKV");
  if (title) {
    const int w = p.textWidth(title);
    p.text(40 + (80 - w) / 2, 3, titleColour, title);
  }
  const NetInfo& n = m.net;
  const int wx = 124;
  switch (n.mode) {
    case NetInfo::Mode::Connected: iconWifi(p, wx, 1, kBlue, wifiLevel(n.rssi)); break;
    case NetInfo::Mode::Connecting: iconWifi(p, wx, 1, kOrange, 1 + static_cast<int>((m.uptimeMs / 400) % 3)); break;
    case NetInfo::Mode::AccessPoint: iconWifi(p, wx, 1, kOrange, 3); break;
    case NetInfo::Mode::Failed: iconWifi(p, wx, 1, kRed, 3); break;
    case NetInfo::Mode::Off: iconWifi(p, wx, 1, kDarkGrey, 0); break;
    case NetInfo::Mode::None: break;
  }
  const uint16_t bc = m.batteryLow || m.batteryPercent < 30 ? kOrange : kGreen;
  iconBattery(p, 141, 2, 17, 8, m.batteryLow && m.batteryPercent > 25 ? 25 : m.batteryPercent, bc, m.batteryValid);
  p.g().drawFastHLine(0, 12, kWidth, kLine);
}

void stateTitle(hkv::CountState s, const char*& t, uint16_t& c) {
  switch (s) {
    case hkv::CountState::Counting: t = "Zählt"; c = kGreen; return;
    case hkv::CountState::Summer: t = "Sommer"; c = kOrange; return;
    case hkv::CountState::SensorError: t = "Fühlerfehler"; c = kRed; return;
    case hkv::CountState::Idle: break;
  }
  t = "Ruhe";
  c = kGrey;
}

// Temperature value in a free font with a small "°C" behind it; right-aligned at xr.
void tempRight(Painter& p, const GFXfont* f, int xr, int baseline, bool valid, float v, const char* unit,
               int unitDy) {
  char b[12];
  if (valid) fmt1(v, b, sizeof(b));
  else snprintf(b, sizeof(b), "--,-");
  const int uw = p.textWidth(unit);
  const int w = p.fontWidth(f, b);
  const int x = xr - uw - 2 - w;
  p.font(f, x, baseline, valid ? kWhite : kGrey, b);
  p.text(xr - uw, baseline - unitDy, valid ? kWhite : kGrey, unit);
}

// ------------------------------------------------------------------ pages

// Consumption: current units large and centred, previous billing period below
// (same structure as the temperature page). The dot before the title shows the state.
void pageOverview(Painter& p, const DisplayModel& m) {
  const char* st;
  uint16_t sc;
  stateTitle(m.alloc.state, st, sc);
  header(p, m, "Verbrauch", kWhite);
  statusDot(p, 40 + (80 - p.textWidth("Verbrauch")) / 2 - 10, 3, sc);

  char b[24];
  fmtUnits(m.currentMilliUnits, b, sizeof(b));
  const GFXfont* big = &FreeSansBold18pt7b;
  if (p.fontWidth(big, b) > kWidth - 30) big = &FreeSansBold12pt7b;
  const int w = p.fontWidth(big, b);
  const int x = (kWidth - w - 4 - 11) / 2;
  p.font(big, x, 45, kWhite, b);
  p.text(x + w + 4, 38, kGrey, "VE");

  p.g().drawFastHLine(0, 53, kWidth, kLine);
  if (m.dueDateValid) {
    fmtUnits(m.dueMilliUnits, b, sizeof(b));
    const int lw = p.textWidth("Vorperiode");
    const int vw = p.fontWidth(&FreeSansBold9pt7b, b);
    const int total = lw + 6 + vw + 3 + 11;
    const int x0 = (kWidth - total) / 2;
    p.text(x0, 63, kGrey, "Vorperiode");
    p.font(&FreeSansBold9pt7b, x0 + lw + 6, 71, kWhite, b);
    p.text(x0 + lw + 6 + vw + 3, 64, kGrey, "VE");
  } else {
    p.center(63, kGrey, "Vorperiode: noch keine");
  }
}

// Temperatures, large (live readings while this page is shown).
void pageTemperatures(Painter& p, const DisplayModel& m) {
  header(p, m, "Temperaturen", m.live ? kGreen : kWhite);
  iconThermometer(p, 3, kTop + 2, kRed);
  p.text(18, kTop + 1, kGrey, "Heizkörper");
  tempRight(p, &FreeSansBold12pt7b, 78, 46, m.radiatorValid, m.alloc.radiatorC, "°C", 10);
  iconThermometer(p, 84, kTop + 2, kBlue);
  p.text(99, kTop + 1, kGrey, "Raum");
  tempRight(p, &FreeSansBold12pt7b, kWidth - 2, 46, m.roomValid, m.alloc.roomC, "°C", 10);
  p.g().drawFastVLine(80, kTop, 36, kDarkGrey);
  p.g().drawFastHLine(0, 53, kWidth, m.live ? kGreen : kLine);

  char b[16], line[24];
  fmt1(m.alloc.deltaK, b, sizeof(b));
  snprintf(line, sizeof(line), "%s K", b);
  const int w = 14 + p.fontWidth(&FreeSansBold12pt7b, line);
  const int x = (kWidth - w) / 2;
  iconDelta(p, x, 60, kWhite, 9);
  p.font(&FreeSansBold12pt7b, x + 14, 76, kWhite, line);
}

void pageConnected(Painter& p, const DisplayModel& m) {
  const NetInfo& n = m.net;
  char b[40];
  header(p, m, "Verbindung", kWhite);
  iconWifi(p, 2, kTop, kBlue, wifiLevel(n.rssi));
  char ssid[23];
  truncCopy(ssid, sizeof(ssid), n.ssid);
  p.text(18, kTop + 2, kWhite, ssid);
  p.text(3, 28, kGrey, "IP");
  p.compact(18, 28, kWhite, n.ip);
  snprintf(b, sizeof(b), "%d dBm  Kanal %u", n.rssi, n.channel);
  p.text(3, 39, kWhite, b);
  iconCloud(p, 1, 49, kWhite);
  uint16_t c = kDarkGrey;
  if (n.mqtt == NetInfo::Mqtt::Connected) {
    c = kGreen;
    if (n.mqttAgeS == UINT32_MAX) snprintf(b, sizeof(b), "MQTT verbunden");
    else if (n.mqttAgeS < 60) snprintf(b, sizeof(b), "MQTT vor %lu s", static_cast<unsigned long>(n.mqttAgeS));
    else snprintf(b, sizeof(b), "MQTT vor %lu min", static_cast<unsigned long>(n.mqttAgeS / 60));
  } else if (n.mqtt == NetInfo::Mqtt::Disconnected) {
    c = kRed;
    snprintf(b, sizeof(b), "MQTT getrennt");
  } else {
    snprintf(b, sizeof(b), "MQTT aus");
  }
  statusDot(p, 20, 51, c);
  p.text(30, 51, n.mqtt == NetInfo::Mqtt::Disabled ? kGrey : kWhite, b);
  snprintf(b, sizeof(b), "FW %s", n.firmware);
  p.text(3, 64, kGrey, b);
  p.g().setFont(&TomThumb);
  p.g().setTextColor(kGrey);
  p.g().setCursor(70, 70);
  p.g().print(n.mac);
  p.g().setFont(nullptr);
}

// Wi-Fi setup assistant: QR code left, credentials right (no header: the QR needs the height).
void pageSetup(Painter& p, const DisplayModel& m, const QrMatrix* qr) {
  const NetInfo& n = m.net;
  int x0 = 3;
  if (qr && qr->size > 0) {
    const int scale = qr->size * 2 + 4 <= kHeight - 4 ? 2 : 1;
    const int side = qr->size * scale + 4;
    const int y0 = (kHeight - side) / 2;
    p.g().fillRect(2, y0, side, side, kWhite);
    for (int y = 0; y < qr->size; ++y)
      for (int x = 0; x < qr->size; ++x)
        if (qr->get(x, y)) p.g().fillRect(4 + x * scale, y0 + 2 + y * scale, scale, scale, kBlack);
    x0 = 2 + side + 5;
  }
  p.text(x0, 3, kOrange, "WLAN einrichten", true);
  p.text(x0, 17, kGrey, "Netz:");
  p.text(x0, 26, kWhite, n.apSsid, true);
  p.text(x0, 39, kGrey, "Passwort:");
  p.text(x0, 48, kWhite, n.apPass, true);
  p.compact(x0, 63, kWhite, n.apIp);
  p.g().drawFastHLine(x0, 74, kWidth - x0 - 2, kOrange);
}

void pageConnecting(Painter& p, const DisplayModel& m) {
  const NetInfo& n = m.net;
  char b[24];
  header(p, m, "Verbinden", kWhite);
  const int cx = 28, cy = 47, r = 25;
  p.arc(cx, cy, r, 4, 0.0f, 359.0f, kDarkGrey);
  p.arc(cx, cy, r, 4, 0.0f, 3.59f * n.progressPct, kBlue);
  snprintf(b, sizeof(b), "%u/%u", n.attempt, n.maxAttempts);
  const int w = p.fontWidth(&FreeSansBold9pt7b, b);
  p.font(&FreeSansBold9pt7b, cx - w / 2 - 1, cy + 6, kWhite, b);

  p.text(62, kTop + 3, kGrey, "Verbinde mit");
  char ssid[17];
  truncCopy(ssid, sizeof(ssid), n.ssid);
  p.text(62, kTop + 15, kWhite, ssid, true);
  p.text(62, 48, kWhite, "Bitte warten");
  snprintf(b, sizeof(b), "ca. %u s", n.secondsLeft);
  iconClock(p, 62, 61, kGrey);
  p.text(74, 62, kGrey, b);
}

void pageFailed(Painter& p, const DisplayModel& m) {
  const NetInfo& n = m.net;
  header(p, m, "WLAN", kRed);
  iconWarning(p, 21, kTop + 6, 34, kRed);
  p.text(46, kTop + 3, kWhite, "Verbindung", true);
  p.text(46, kTop + 13, kWhite, "fehlgeschlagen!", true);
  char b[32];
  snprintf(b, sizeof(b), "Einrichtung in %u s", n.secondsLeft);
  p.text(46, 41, kGrey, "Zurück zur");
  p.text(46, 51, kWhite, b);
  p.g().fillRect(4, 70, kWidth - 8, 3, kDarkGrey);
  p.g().fillRect(4, 70, (kWidth - 8) * n.progressPct / 100, 3, kBlue);
}

void pageWifiOff(Painter& p, const DisplayModel& m) {
  header(p, m, "Verbindung", kWhite);
  iconWifi(p, 6, kTop + 6, kDarkGrey, 0);
  p.text(26, kTop + 8, kWhite, "WLAN aus", true);
  p.text(6, 40, kGrey, "WLAN-Taste:");
  p.text(80, 40, kWhite, "einschalten");
  p.text(6, 54, kGrey, "Taster 3 s:");
  p.text(80, 54, kWhite, "einrichten");
}

void pageBatteryLow(Painter& p, const DisplayModel& m) {
  header(p, m, "Akku", kOrange);
  Adafruit_GFX& g = p.g();
  const int x = 8, y = kTop + 2;
  g.fillRect(x + 7, y, 8, 3, kOrange);
  g.drawRoundRect(x, y + 3, 22, 56, 3, kOrange);
  g.drawRoundRect(x + 1, y + 4, 20, 54, 3, kOrange);
  g.fillRoundRect(x + 9, y + 14, 4, 22, 1, kOrange);
  g.fillRect(x + 9, y + 41, 4, 5, kOrange);

  p.text(42, kTop + 3, kOrange, "Akkustand niedrig!", true);
  p.text(42, kTop + 15, kWhite, "Bitte bald laden.");
  p.text(42, 46, kGrey, "Aktuell");
  char b[24];
  fmtUnits(m.currentMilliUnits, b, sizeof(b));
  const int w = p.font(&FreeSansBold12pt7b, 42, 76, kWhite, b);
  p.text(42 + w + 3, 69, kGrey, "VE");
}

}  // namespace

void render(Adafruit_GFX& g, const DisplayModel& m, const QrMatrix* qr) {
  g.fillScreen(kBlack);
  g.setTextWrap(false);
  g.cp437(true);
  Painter p(g);
  switch (m.page) {
    case Page::Consumption: pageOverview(p, m); break;
    case Page::Temperatures: pageTemperatures(p, m); break;
    case Page::BatteryLow: pageBatteryLow(p, m); break;
    case Page::Network:
    case Page::Count:
      switch (m.net.mode) {
        case NetInfo::Mode::Connected: pageConnected(p, m); break;
        case NetInfo::Mode::AccessPoint: pageSetup(p, m, qr); break;
        case NetInfo::Mode::Connecting: pageConnecting(p, m); break;
        case NetInfo::Mode::Failed: pageFailed(p, m); break;
        case NetInfo::Mode::Off: pageWifiOff(p, m); break;
        case NetInfo::Mode::None:
          header(p, m, "Verbindung", kWhite);
          p.center(40, kGrey, "keine Verbindung");
          break;
      }
      break;
  }
}

}  // namespace ui

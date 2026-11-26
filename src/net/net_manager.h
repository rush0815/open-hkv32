#pragma once
// WiFi, setup portal (captive AP with QR code), web UI / JSON API, NTP and MQTT
// for the ESP32 prototype. Not part of the portable core: the CC1312 device is
// configured via wM-Bus / optical interface instead. All meter configuration goes
// through HkvApp::applySettings().

#include <Arduino.h>
#include <DNSServer.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <WiFi.h>

#include "hkv/app.h"
#include "hkv/display_model.h"
#include "hkv/hal.h"
#include "hkv/schedule.h"
#include "ui/ui_render.h"

namespace net {

struct NetSettings {
  uint8_t wifiEnabled;
  char ssid[33];
  char pass[65];
  char hostname[25];
  char ntpServer[49];
  char adminPass[33];  // optional web login (user "admin"); empty = no login required
  uint8_t mqttEnabled;
  uint8_t mqttDiscovery;  // Home Assistant MQTT discovery
  uint16_t mqttPort;
  char mqttHost[65];
  char mqttUser[33];
  char mqttPass[65];
  char mqttTopic[49];     // base topic, e.g. hkv/12345678
  char apPass[33];        // WPA2 password of the setup access point (random per device)
};

// Power options of the ESP32 prototype (stored separately under the key "power").
struct PowerSettings {
  enum WifiMode : uint8_t { Always = 0, OnDemand = 1, Off = 2 };
  uint8_t wifiMode = Always;  // OnDemand: WiFi only for the scheduled transfers; Off: only via WiFi button
  uint8_t wifiPs = 1;         // modem sleep while connected: 0 off, 1 min, 2 max
  uint16_t wakeMin = 10;      // WiFi on-time after the WiFi button (or after changing the mode)
  uint16_t cpuMhz = 240;      // 80 / 160 / 240
  uint8_t lightSleep = 1;     // light sleep between measurements while WiFi and display are off
};

class NetManager {
 public:
  NetManager(hkv::HkvApp& app, hkv::IStorage& storage)
      : app_(app), storage_(storage), server_(80), mqtt_(mqttSocket_) {}

  void begin();
  void loop();
  void startPortal();
  // WiFi button: switch WiFi on for PowerSettings::wakeMin minutes (setup mode if unconfigured).
  void wake();
  const PowerSettings& power() const { return pw_; }

  bool busy() const { return state_ != State::Off; }  // WiFi radio is on -> no light sleep
  void fillNetInfo(hkv::NetInfo& info);
  const ui::QrMatrix& qr() const { return qr_; }

  // True once after a state change the user should see (connecting, failed, setup).
  bool takeAttention() {
    const bool a = attention_;
    attention_ = false;
    return a;
  }

  static constexpr uint8_t kMaxAttempts = 3;
  static constexpr uint32_t kAttemptMs = 10000;
  static constexpr uint32_t kFailedHoldMs = 5000;

 private:
  enum class State { Off, Connecting, Connected, Failed, Portal };

  void loadSettings();
  bool saveSettings();
  void startStation(bool userVisible);
  void stopWifi();
  void setState(State s, bool userVisible);
  void setupRoutes();
  void startServer();
  bool requireAuth();
  void buildQr();
  void applyPower();
  bool wakeActive() const { return wakeArmed_ && static_cast<int32_t>(wakeUntil_ - millis()) > 0; }
  void powerLoop(uint32_t now);

  void mqttLoop();
  bool mqttConnect();
  void mqttPublishDiscovery();
  void mqttPublishState();
  bool transferDue();
  int32_t secondsToNextTransfer();
  String mqttBase() const;

  void statusJson(String& j) const;
  void sendIndex();
  void sendJson(int code, const String& body);
  void handleConfigGet();
  void handleConfigSave();
  void handleScan();
  void handleLive();
  void handleSetTime();
  void handleAction();
  void handleApiStatus();
  void handleApiTelegram();
  void handleNotFound();

  String apSsid() const;

  hkv::HkvApp& app_;
  hkv::IStorage& storage_;
  WebServer server_;
  DNSServer dns_;
  WiFiClient mqttSocket_;
  PubSubClient mqtt_;
  NetSettings ns_{};
  ui::QrMatrix qr_;

  State state_ = State::Off;
  uint32_t stateSince_ = 0;
  uint8_t attempt_ = 0;
  uint32_t attemptSince_ = 0;
  bool attention_ = false;
  bool serverStarted_ = false;
  bool restartPending_ = false;
  uint32_t restartAt_ = 0;

  uint32_t mqttLastTry_ = 0;
  uint32_t mqttLastPublish_ = 0;
  bool mqttPublishedOnce_ = false;
  PowerSettings pw_;
  bool wakeArmed_ = false;
  bool powerApplyPending_ = false;  // apply CPU clock etc. only after the HTTP response is out
  uint32_t powerApplyAt_ = 0;
  uint32_t wakeUntil_ = 0;
  bool sessionTransfer_ = false;     // WiFi was switched on only for a scheduled transfer
  bool publishedInSession_ = false;
  uint32_t sessionStart_ = 0;
  uint32_t connectedAt_ = 0;
  uint32_t nextSessionAllowed_ = 0;  // back-off after a failed transfer session
  hkv::TransferSchedule xfer_;   // when the state is sent via WiFi (MQTT)
  int32_t nextDueIdx_ = -1;      // Times mode: next local minute index, -1 = not planned
};

}  // namespace net

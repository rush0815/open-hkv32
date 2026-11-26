#include "net/net_manager.h"

#include <ESPmDNS.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <qrcode.h>  // esp_qrcode (ESP-IDF component, part of the Arduino core)

#include "hkv/log.h"
#include "hkv/oms.h"
#include "hkv/persist.h"
#include "hkv_features.h"
#include "net/web_index.h"

namespace net {

namespace {

constexpr const char* kKeyNet = "net";
constexpr uint16_t kNetVersion = 3;

// Layout of version 2 (setup AP and web login shared one password) - for migration.
struct NetSettingsV2 {
  uint8_t wifiEnabled;
  char ssid[33];
  char pass[65];
  char hostname[25];
  char ntpServer[49];
  char adminPass[33];
  uint8_t mqttEnabled;
  uint8_t mqttDiscovery;
  uint16_t mqttPort;
  char mqttHost[65];
  char mqttUser[33];
  char mqttPass[65];
  char mqttTopic[49];
};
constexpr uint32_t kMqttRetryMs = 15000;
constexpr const char* kKeyXfer = "xfer";
constexpr uint16_t kXferVersion = 1;
constexpr const char* kKeyPower = "power";
constexpr uint16_t kPowerVersion = 1;
constexpr uint32_t kSessionTimeoutMs = 60000;    // max. WiFi on-time for one scheduled transfer
constexpr uint32_t kSessionBackoffMs = 300000;   // wait after a failed transfer session
constexpr uint32_t kFallbackIntervalMin = 60;  // Times mode without a valid clock

void copyStr(char* dst, size_t cap, const String& src) {
  strncpy(dst, src.c_str(), cap - 1);
  dst[cap - 1] = '\0';
}

void randomPassword(char* out, size_t len) {
  static const char kChars[] = "abcdefghjkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";  // no 0/O/1/l/I
  for (size_t i = 0; i < len; ++i) out[i] = kChars[esp_random() % (sizeof(kChars) - 1)];
  out[len] = '\0';
}

// Escaping for the "WIFI:" QR code payload.
String qrEsc(const char* s) {
  String o;
  for (; *s; ++s) {
    if (*s == '\\' || *s == ';' || *s == ',' || *s == ':' || *s == '"') o += '\\';
    o += *s;
  }
  return o;
}

String jsonEsc(const char* s) {
  String o;
  for (; *s; ++s) {
    if (*s == '"' || *s == '\\') o += '\\';
    if (static_cast<unsigned char>(*s) >= 0x20) o += *s;
  }
  return o;
}

String fmtFloat(float v, int decimals) { return String(v, decimals); }

// Units with one decimal, cut off (not rounded) - identical to the display.
String fmtUnits(uint64_t milli) {
  const uint64_t tenths = milli / 100u;
  return String(static_cast<unsigned long>(tenths / 10u)) + "." + String(static_cast<unsigned>(tenths % 10u));
}

bool parseHexKey(const String& hex, uint8_t out[16]) {
  if (hex.length() != 32) return false;
  for (int i = 0; i < 16; ++i) {
    char b[3] = {hex[2 * i], hex[2 * i + 1], 0};
    char* end = nullptr;
    const long v = strtol(b, &end, 16);
    if (*end != '\0') return false;
    out[i] = static_cast<uint8_t>(v);
  }
  return true;
}

ui::QrMatrix* g_qrTarget = nullptr;

void qrCopy(esp_qrcode_handle_t h) {
  const int n = esp_qrcode_get_size(h);
  if (!g_qrTarget) return;
  if (n > ui::QrMatrix::kMax) {
    g_qrTarget->size = 0;
    return;
  }
  g_qrTarget->size = static_cast<uint8_t>(n);
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) g_qrTarget->set(x, y, esp_qrcode_get_module(h, x, y));
}

}  // namespace

// ---------------------------------------------------------------- lifecycle

void NetManager::begin() {
  loadSettings();
  setupRoutes();
  mqtt_.setBufferSize(1024);
  applyPower();
  if (!HKV_ENABLE_WIFI) return;
  if (ns_.ssid[0] == '\0') {
    startPortal();  // nothing configured yet: stay in setup mode until configured
  } else if (ns_.wifiEnabled && pw_.wifiMode == PowerSettings::Always) {
    startStation(true);
  }
  // OnDemand: powerLoop() connects for the first transfer (also fetches the time).
  // Off: WiFi only via the WiFi button.
}

void NetManager::applyPower() {
  const uint32_t mhz = pw_.cpuMhz == 80 || pw_.cpuMhz == 160 ? pw_.cpuMhz : 240;
  if (getCpuFrequencyMhz() != mhz) setCpuFrequencyMhz(mhz);
  if (state_ == State::Connected)
    WiFi.setSleep(pw_.wifiPs == 0 ? WIFI_PS_NONE : pw_.wifiPs == 2 ? WIFI_PS_MAX_MODEM : WIFI_PS_MIN_MODEM);
}

void NetManager::wake() {
  if (!HKV_ENABLE_WIFI) return;
  wakeArmed_ = true;
  wakeUntil_ = millis() + pw_.wakeMin * 60000UL;
  HKV_LOG("wifi: woken for %u min", pw_.wakeMin);
  if (ns_.ssid[0] == '\0') {
    if (state_ != State::Portal) startPortal();
    else attention_ = true;
    return;
  }
  if (state_ == State::Off || state_ == State::Failed) startStation(true);
  else attention_ = true;  // already on: just show the connection page
}

// WiFi on/off according to the power mode (called every loop).
void NetManager::powerLoop(uint32_t now) {
  if (!HKV_ENABLE_WIFI || pw_.wifiMode == PowerSettings::Always || wakeActive()) return;
  if (state_ == State::Off) {
    const bool mqttOn = ns_.mqttEnabled && ns_.mqttHost[0];
    if (pw_.wifiMode == PowerSettings::OnDemand && ns_.ssid[0] && ns_.wifiEnabled && mqttOn &&
        static_cast<int32_t>(now - nextSessionAllowed_) >= 0 && transferDue()) {
      HKV_LOG("wifi: on for scheduled transfer");
      sessionTransfer_ = true;
      publishedInSession_ = false;
      sessionStart_ = now;
      startStation(false);
    }
    return;
  }
  if (state_ == State::Portal) return;  // setup mode has its own timeout
  if (sessionTransfer_) {
    hkv::DateTime lt;
    const bool timeOk = app_.localTime(lt) || (state_ == State::Connected && now - connectedAt_ > 15000);
    const bool done = publishedInSession_ && timeOk;
    if (done || now - sessionStart_ > kSessionTimeoutMs) {
      if (!done) {
        HKV_LOG("wifi: transfer session failed, next try in %lu min", kSessionBackoffMs / 60000UL);
        nextSessionAllowed_ = now + kSessionBackoffMs;
      }
      sessionTransfer_ = false;
      stopWifi();
    }
    return;
  }
  stopWifi();  // wake window over (or mode just changed)
}

void NetManager::loadSettings() {
  bool dirty = false;
  NetSettingsV2 old;
  if (hkv::loadObject(storage_, kKeyNet, kNetVersion, ns_)) {
    // current format
  } else if (hkv::loadObject(storage_, kKeyNet, 2, old)) {
    // v2 -> v3: the old shared password becomes the AP password (unchanged for the user),
    // the web login starts empty and can be set later.
    memset(&ns_, 0, sizeof(ns_));
    ns_.wifiEnabled = old.wifiEnabled;
    memcpy(ns_.ssid, old.ssid, sizeof(ns_.ssid));
    memcpy(ns_.pass, old.pass, sizeof(ns_.pass));
    memcpy(ns_.hostname, old.hostname, sizeof(ns_.hostname));
    memcpy(ns_.ntpServer, old.ntpServer, sizeof(ns_.ntpServer));
    memcpy(ns_.apPass, old.adminPass, sizeof(ns_.apPass));
    ns_.mqttEnabled = old.mqttEnabled;
    ns_.mqttDiscovery = old.mqttDiscovery;
    ns_.mqttPort = old.mqttPort;
    memcpy(ns_.mqttHost, old.mqttHost, sizeof(ns_.mqttHost));
    memcpy(ns_.mqttUser, old.mqttUser, sizeof(ns_.mqttUser));
    memcpy(ns_.mqttPass, old.mqttPass, sizeof(ns_.mqttPass));
    memcpy(ns_.mqttTopic, old.mqttTopic, sizeof(ns_.mqttTopic));
    HKV_LOG("net settings migrated (v2 -> v3)");
    dirty = true;
  } else {
    memset(&ns_, 0, sizeof(ns_));
    ns_.wifiEnabled = 1;
    snprintf(ns_.hostname, sizeof(ns_.hostname), "hkv-%08lu", static_cast<unsigned long>(app_.settings().meterId));
    copyStr(ns_.ntpServer, sizeof(ns_.ntpServer), "pool.ntp.org");
    ns_.mqttPort = 1883;
    ns_.mqttDiscovery = 1;
    snprintf(ns_.mqttTopic, sizeof(ns_.mqttTopic), "hkv/%08lu", static_cast<unsigned long>(app_.settings().meterId));
    dirty = true;
  }
  // Every device gets its own random setup-AP password (shown on the display / QR code).
  if (strlen(ns_.apPass) < 8 || strcmp(ns_.apPass, "hkv-setup") == 0) {
    randomPassword(ns_.apPass, 8);
    dirty = true;
  }
  if (dirty) saveSettings();
  if (!hkv::loadObject(storage_, kKeyXfer, kXferVersion, xfer_)) xfer_ = hkv::TransferSchedule();  // every 15 min
  if (!hkv::loadObject(storage_, kKeyPower, kPowerVersion, pw_)) pw_ = PowerSettings();
}

bool NetManager::saveSettings() { return hkv::saveObject(storage_, kKeyNet, kNetVersion, ns_); }

String NetManager::apSsid() const {
  char b[16];
  snprintf(b, sizeof(b), "HKV-%08lu", static_cast<unsigned long>(app_.settings().meterId));
  return String(b);
}

void NetManager::setState(State s, bool userVisible) {
  state_ = s;
  stateSince_ = millis();
  if (userVisible) attention_ = true;
}

void NetManager::startServer() {
  if (serverStarted_) return;
  server_.begin();
  serverStarted_ = true;
}

void NetManager::startStation(bool userVisible) {
  dns_.stop();
  if (WiFi.getMode() & WIFI_AP) WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(ns_.hostname);
  WiFi.begin(ns_.ssid, ns_.pass);
  attempt_ = 1;
  attemptSince_ = millis();
  setState(State::Connecting, userVisible);
  startServer();
  HKV_LOG("wifi: connecting to '%s' (attempt 1/%u)", ns_.ssid, kMaxAttempts);
}

void NetManager::buildQr() {
  const String payload = "WIFI:T:WPA;S:" + qrEsc(apSsid().c_str()) + ";P:" + qrEsc(ns_.apPass) + ";;";
  esp_qrcode_config_t cfg{};
  cfg.display_func = qrCopy;
  cfg.max_qrcode_version = 6;
  cfg.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;
  g_qrTarget = &qr_;
  if (esp_qrcode_generate(&cfg, payload.c_str()) != ESP_OK) qr_.size = 0;
  g_qrTarget = nullptr;
}

void NetManager::startPortal() {
  if (!HKV_ENABLE_WIFI) return;
  if (mqtt_.connected()) mqtt_.disconnect();
  if (WiFi.getMode() != WIFI_OFF) WiFi.disconnect(true);
  WiFi.mode(WIFI_AP_STA);  // STA part only for the network scan of the setup page
  if (!WiFi.softAP(apSsid().c_str(), ns_.apPass)) HKV_LOG("wifi: softAP start FAILED");
  dns_.setErrorReplyCode(DNSReplyCode::NoError);
  dns_.start(53, "*", WiFi.softAPIP());
  buildQr();
  setState(State::Portal, true);
  startServer();
  HKV_LOG("wifi: setup portal '%s' (ch %d) on http://%s (QR %ux%u)", WiFi.softAPSSID().c_str(),
          static_cast<int>(WiFi.channel()), WiFi.softAPIP().toString().c_str(), qr_.size, qr_.size);
}

void NetManager::stopWifi() {
  dns_.stop();
  if (mqtt_.connected()) mqtt_.disconnect();
  if (serverStarted_) {
    server_.stop();
    serverStarted_ = false;
  }
  MDNS.end();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  setState(State::Off, false);
  HKV_LOG("wifi: off");
}

void NetManager::loop() {
  if (restartPending_ && static_cast<int32_t>(millis() - restartAt_) >= 0) ESP.restart();
  if (powerApplyPending_ && static_cast<int32_t>(millis() - powerApplyAt_) >= 0) {
    powerApplyPending_ = false;
    applyPower();
    HKV_LOG("power: cpu %lu MHz, wifi mode %u, modem sleep %u, light sleep %u",
            static_cast<unsigned long>(getCpuFrequencyMhz()), pw_.wifiMode, pw_.wifiPs, pw_.lightSleep);
  }
  powerLoop(millis());
  if (state_ == State::Off) return;

  server_.handleClient();
  const uint32_t now = millis();

  switch (state_) {
    case State::Portal:
      dns_.processNextRequest();
      // Without any configured network the portal stays open; otherwise retry the
      // station connection after the timeout, unless someone is still connected.
      if (ns_.ssid[0] && now - stateSince_ > HKV_PORTAL_TIMEOUT_MS) {
        if (WiFi.softAPgetStationNum() > 0) stateSince_ = now;
        else if (ns_.wifiEnabled && pw_.wifiMode == PowerSettings::Always) startStation(false);
        else stopWifi();
      }
      break;

    case State::Connecting:
      if (WiFi.status() == WL_CONNECTED) {
        setState(State::Connected, false);
        connectedAt_ = now;
        applyPower();                     // modem sleep level
        configTime(0, 0, ns_.ntpServer);  // system clock in UTC, local time is done by the core
        if (MDNS.begin(ns_.hostname)) MDNS.addService("http", "tcp", 80);
        mqttLastTry_ = now - kMqttRetryMs;  // connect MQTT right away
        HKV_LOG("wifi: connected, http://%s/ (%s.local), ch %d, %d dBm", WiFi.localIP().toString().c_str(),
                ns_.hostname, static_cast<int>(WiFi.channel()), static_cast<int>(WiFi.RSSI()));
      } else if (now - attemptSince_ > kAttemptMs) {
        if (attempt_ < kMaxAttempts) {
          ++attempt_;
          attemptSince_ = now;
          WiFi.disconnect();
          WiFi.begin(ns_.ssid, ns_.pass);
          HKV_LOG("wifi: attempt %u/%u", attempt_, kMaxAttempts);
        } else {
          HKV_LOG("wifi: connection failed");
          WiFi.disconnect();
          if (pw_.wifiMode != PowerSettings::Always && !wakeActive()) {
            // power saving: no setup portal on a failed background connection
            sessionTransfer_ = false;
            nextSessionAllowed_ = now + kSessionBackoffMs;
            stopWifi();
          } else {
            setState(State::Failed, true);
          }
        }
      }
      break;

    case State::Failed:
      if (now - stateSince_ > kFailedHoldMs) startPortal();
      break;

    case State::Connected:
      if (WiFi.status() != WL_CONNECTED) {
        HKV_LOG("wifi: connection lost");
        attempt_ = 1;
        attemptSince_ = now;
        setState(State::Connecting, false);  // the WiFi driver reconnects by itself
      } else {
        mqttLoop();
      }
      break;

    case State::Off:
      break;
  }
}

void NetManager::fillNetInfo(hkv::NetInfo& n) {
  using Mode = hkv::NetInfo::Mode;
  n = hkv::NetInfo();
  if (!HKV_ENABLE_WIFI) return;
  const uint32_t now = millis();
  switch (state_) {
    case State::Off: n.mode = Mode::Off; break;
    case State::Connecting: n.mode = Mode::Connecting; break;
    case State::Connected: n.mode = Mode::Connected; break;
    case State::Failed: n.mode = Mode::Failed; break;
    case State::Portal: n.mode = Mode::AccessPoint; break;
  }
  copyStr(n.ssid, sizeof(n.ssid), ns_.ssid);
  if (state_ == State::Connected) {
    copyStr(n.ip, sizeof(n.ip), WiFi.localIP().toString());
    n.channel = static_cast<uint8_t>(WiFi.channel());
    n.rssi = static_cast<int8_t>(WiFi.RSSI());
  }
  if (state_ == State::Connecting) {
    const uint32_t inAttempt = min(now - attemptSince_, kAttemptMs);
    n.attempt = attempt_;
    n.maxAttempts = kMaxAttempts;
    n.progressPct = static_cast<uint8_t>(((attempt_ - 1) * kAttemptMs + inAttempt) * 100 / (kMaxAttempts * kAttemptMs));
    n.secondsLeft = static_cast<uint8_t>((kAttemptMs - inAttempt + 999) / 1000);
  } else if (state_ == State::Failed) {
    const uint32_t el = min(now - stateSince_, kFailedHoldMs);
    n.progressPct = static_cast<uint8_t>(el * 100 / kFailedHoldMs);
    n.secondsLeft = static_cast<uint8_t>((kFailedHoldMs - el + 999) / 1000);
  }
  copyStr(n.apSsid, sizeof(n.apSsid), apSsid());
  copyStr(n.apPass, sizeof(n.apPass), ns_.apPass);
  copyStr(n.apIp, sizeof(n.apIp), WiFi.softAPIP().toString());

  if (!ns_.mqttEnabled || !ns_.mqttHost[0]) n.mqtt = hkv::NetInfo::Mqtt::Disabled;
  else if (mqtt_.connected()) n.mqtt = hkv::NetInfo::Mqtt::Connected;
  else n.mqtt = hkv::NetInfo::Mqtt::Disconnected;
  n.mqttAgeS = mqttPublishedOnce_ ? (now - mqttLastPublish_) / 1000 : UINT32_MAX;

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(n.mac, sizeof(n.mac), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  copyStr(n.firmware, sizeof(n.firmware), HKV_FIRMWARE_VERSION);
}

// ---------------------------------------------------------------- MQTT

String NetManager::mqttBase() const {
  if (ns_.mqttTopic[0]) return String(ns_.mqttTopic);
  char b[24];
  snprintf(b, sizeof(b), "hkv/%08lu", static_cast<unsigned long>(app_.settings().meterId));
  return String(b);
}

void NetManager::mqttLoop() {
  if (!ns_.mqttEnabled || !ns_.mqttHost[0]) return;
  const uint32_t now = millis();
  if (!mqtt_.connected()) {
    if (now - mqttLastTry_ >= kMqttRetryMs) {
      mqttLastTry_ = now;
      mqttConnect();
    }
    return;
  }
  mqtt_.loop();
  if (transferDue()) mqttPublishState();
}

// Interval mode: every N minutes after the last transfer.
// Times mode: at the configured local times (needs a valid clock, otherwise hourly).
bool NetManager::transferDue() {
  hkv::DateTime lt;
  if (xfer_.mode == hkv::TransferSchedule::Times && app_.localTime(lt)) {
    const int32_t idx = hkv::localMinuteIndex(lt);
    if (nextDueIdx_ < 0) nextDueIdx_ = hkv::nextScheduledMinute(xfer_, idx);
    return idx >= nextDueIdx_;
  }
  const uint32_t ivMin = xfer_.mode == hkv::TransferSchedule::Times ? kFallbackIntervalMin : xfer_.intervalMin;
  return !mqttPublishedOnce_ || millis() - mqttLastPublish_ >= ivMin * 60000UL;
}

int32_t NetManager::secondsToNextTransfer() {
  hkv::DateTime lt;
  if (xfer_.mode == hkv::TransferSchedule::Times && app_.localTime(lt)) {
    const int32_t idx = hkv::localMinuteIndex(lt);
    const int32_t next = nextDueIdx_ >= 0 ? nextDueIdx_ : hkv::nextScheduledMinute(xfer_, idx);
    const int32_t s = (next - idx) * 60 - lt.second;
    return s > 0 ? s : 0;
  }
  if (!mqttPublishedOnce_) return 0;
  const uint32_t ivMin = xfer_.mode == hkv::TransferSchedule::Times ? kFallbackIntervalMin : xfer_.intervalMin;
  const int32_t left = static_cast<int32_t>(ivMin * 60UL) - static_cast<int32_t>((millis() - mqttLastPublish_) / 1000);
  return left > 0 ? left : 0;
}

bool NetManager::mqttConnect() {
  mqtt_.setServer(ns_.mqttHost, ns_.mqttPort ? ns_.mqttPort : 1883);
  char clientId[24];
  snprintf(clientId, sizeof(clientId), "hkv-%08lu", static_cast<unsigned long>(app_.settings().meterId));
  const String will = mqttBase() + "/status";
  const bool ok = mqtt_.connect(clientId, ns_.mqttUser[0] ? ns_.mqttUser : nullptr,
                                ns_.mqttPass[0] ? ns_.mqttPass : nullptr, will.c_str(), 0, true, "offline");
  if (!ok) {
    HKV_LOG("mqtt: connect to %s:%u failed (state %d)", ns_.mqttHost, ns_.mqttPort, mqtt_.state());
    return false;
  }
  HKV_LOG("mqtt: connected to %s:%u, base topic %s", ns_.mqttHost, ns_.mqttPort, mqttBase().c_str());
  mqtt_.publish(will.c_str(), "online", true);
  if (ns_.mqttDiscovery) mqttPublishDiscovery();
  mqttPublishState();
  return true;
}

void NetManager::mqttPublishState() {
  String j;
  statusJson(j);
  const String topic = mqttBase() + "/state";
  if (mqtt_.publish(topic.c_str(), j.c_str(), true)) {
    mqttLastPublish_ = millis();
    mqttPublishedOnce_ = true;
    publishedInSession_ = true;
    hkv::DateTime lt;
    if (xfer_.mode == hkv::TransferSchedule::Times && app_.localTime(lt))
      nextDueIdx_ = hkv::nextScheduledMinute(xfer_, hkv::localMinuteIndex(lt));
  } else {
    HKV_LOG("mqtt: publish failed (%u bytes)", j.length());
  }
}

void NetManager::mqttPublishDiscovery() {
  struct Sensor {
    const char* key;
    const char* name;
    const char* tpl;
    const char* unit;
    const char* devClass;
    const char* stateClass;
  };
  static const Sensor kSensors[] = {
      {"units", "Verbrauch", "{{ value_json.units }}", "VE", nullptr, "total_increasing"},
      {"radiator", "Heizkörper", "{{ value_json.radiator_c }}", "°C", "temperature", "measurement"},
      {"room", "Raumtemperatur", "{{ value_json.room_c }}", "°C", "temperature", "measurement"},
      {"pressure", "Luftdruck", "{{ value_json.pressure_hpa }}", "hPa", "atmospheric_pressure", "measurement"},
      {"delta", "Delta T", "{{ value_json.delta_k }}", "K", nullptr, "measurement"},
      {"battery", "Akku", "{{ value_json.battery_pct }}", "%", "battery", "measurement"},
      {"rssi", "WLAN-Signal", "{{ value_json.rssi }}", "dBm", "signal_strength", "measurement"},
      {"state", "Zustand", "{{ value_json.state }}", nullptr, nullptr, nullptr},
  };
  char id[16];
  snprintf(id, sizeof(id), "%08lu", static_cast<unsigned long>(app_.settings().meterId));
  const String base = mqttBase();
  for (const Sensor& s : kSensors) {
    String p;
    p.reserve(600);
    p += "{\"name\":\"";
    p += s.name;
    p += "\",\"uniq_id\":\"hkv_";
    p += id;
    p += '_';
    p += s.key;
    p += "\",\"stat_t\":\"" + base + "/state\",\"avty_t\":\"" + base + "/status\",\"val_tpl\":\"";
    p += s.tpl;
    p += '"';
    if (s.unit) p += String(",\"unit_of_meas\":\"") + s.unit + '"';
    if (s.devClass) p += String(",\"dev_cla\":\"") + s.devClass + '"';
    if (s.stateClass) p += String(",\"stat_cla\":\"") + s.stateClass + '"';
    p += ",\"dev\":{\"ids\":[\"hkv_";
    p += id;
    p += "\"],\"name\":\"HKV ";
    p += id;
    p += "\",\"mf\":\"HKV open source\",\"mdl\":\"TinyPICO prototype\",\"sw\":\"" HKV_FIRMWARE_VERSION "\"}}";
    const String topic = String("homeassistant/sensor/hkv_") + id + "/" + s.key + "/config";
    mqtt_.publish(topic.c_str(), p.c_str(), true);
  }
}

// ---------------------------------------------------------------- HTTP
// The UI is a single static page (src/net/web/index.html, embedded gzip-compressed)
// that talks to the JSON endpoints below.

bool NetManager::requireAuth() {
  // Setup mode: whoever is connected knows the AP password from the device display
  // (physical access) - no extra login. This is also the recovery path for a
  // forgotten web password (hold the button for 3 s).
  if (state_ == State::Portal) return true;
  if (ns_.adminPass[0] == '\0') return true;  // no web password set (yet)
  if (server_.authenticate("admin", ns_.adminPass)) return true;
  server_.requestAuthentication(BASIC_AUTH, "HKV");
  return false;
}

void NetManager::setupRoutes() {
  server_.on("/", HTTP_GET, [this] { sendIndex(); });
  server_.on("/config", HTTP_GET, [this] { sendIndex(); });  // old bookmarks
  server_.on("/api/status", HTTP_GET, [this] { handleApiStatus(); });
  server_.on("/api/telegram", HTTP_GET, [this] { handleApiTelegram(); });
  server_.on("/api/config", HTTP_GET, [this] { handleConfigGet(); });
  server_.on("/api/config", HTTP_POST, [this] { handleConfigSave(); });
  server_.on("/api/time", HTTP_POST, [this] { handleSetTime(); });
  server_.on("/api/action", HTTP_POST, [this] { handleAction(); });
  server_.on("/api/scan", HTTP_GET, [this] { handleScan(); });
  server_.on("/api/live", HTTP_GET, [this] { handleLive(); });
  server_.onNotFound([this] { handleNotFound(); });
}

void NetManager::sendIndex() {
  server_.sendHeader("Content-Encoding", "gzip");
  server_.sendHeader("Cache-Control", "no-cache");
  server_.send_P(200, "text/html; charset=utf-8", reinterpret_cast<const char*>(kWebIndexGz), kWebIndexGzLen);
}

void NetManager::sendJson(int code, const String& body) {
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(code, "application/json; charset=utf-8", body);
}

static String jsonResult(bool ok, const String& msg, bool restart = false) {
  String j = ok ? "{\"ok\":true" : "{\"ok\":false";
  if (msg.length()) j += String(ok ? ",\"message\":\"" : ",\"error\":\"") + jsonEsc(msg.c_str()) + "\"";
  if (restart) j += ",\"restart\":true";
  j += "}";
  return j;
}

void NetManager::handleNotFound() {
  if (state_ == State::Portal) {  // captive portal: send every unknown URL to the start page
    server_.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
    server_.send(302, "text/plain", "");
    return;
  }
  server_.send(404, "text/plain", "not found");
}

void NetManager::handleConfigGet() {
  if (!requireAuth()) return;
  const hkv::Settings& s = app_.settings();
  bool keySet = false;
  for (uint8_t b : s.aesKey) keySet |= b != 0;
  String j;
  j.reserve(900);
  char b[160];
  snprintf(b, sizeof(b), "{\"meterId\":%lu,\"mfct\":\"%s\",\"ver\":%u,\"scale\":%u,",
           static_cast<unsigned long>(s.meterId), s.manufacturer, s.deviceVersion, static_cast<unsigned>(s.scale));
  j += b;
  snprintf(b, sizeof(b), "\"kc\":%g,\"kq\":%g,\"exp\":%g,\"dtn\":%g,\"dts\":%g,\"tmin\":%g,\"tsum\":%g,",
           static_cast<double>(s.kc), static_cast<double>(s.kq), static_cast<double>(s.exponent),
           static_cast<double>(s.nominalDeltaK), static_cast<double>(s.startDeltaK),
           static_cast<double>(s.minRadiatorC), static_cast<double>(s.summerMinRadiatorC));
  j += b;
  snprintf(b, sizeof(b), "\"sumS\":%u,\"sumE\":%u,\"offR\":%g,\"offA\":%g,\"dueD\":%u,\"dueM\":%u,\"emis\":%.2f,\"rcpl\":%.2f,\"rtau\":%.0f,",
           s.summerStartMonth, s.summerEndMonth, static_cast<double>(s.radiatorOffsetC),
           static_cast<double>(s.roomOffsetC), s.dueDay, s.dueMonth, static_cast<double>(s.emissivity), static_cast<double>(s.roomCoupling),
           static_cast<double>(s.roomCouplingTauMin));
  j += b;
  snprintf(b, sizeof(b), "\"mint\":%u,\"tint\":%u,\"utco\":%d,\"dst\":%u,\"batlow\":%u,\"dto\":%u,\"sec\":%u,\"key_set\":%s,",
           s.measureIntervalS, s.telegramIntervalS, s.utcOffsetMin, s.euDst, s.batteryLowMv, s.displayTimeoutS,
           s.omsSecurity, keySet ? "true" : "false");
  j += b;
  j += "\"wifi\":" + String(ns_.wifiEnabled) + ",\"ssid\":\"" + jsonEsc(ns_.ssid) + "\",\"pass_set\":" +
       (ns_.pass[0] ? "true" : "false") + ",\"host\":\"" + jsonEsc(ns_.hostname) + "\",\"ntp\":\"" +
       jsonEsc(ns_.ntpServer) + "\",";
  j += "\"mqtt\":" + String(ns_.mqttEnabled) + ",\"mhost\":\"" + jsonEsc(ns_.mqttHost) + "\",\"mport\":" +
       String(ns_.mqttPort) + ",\"muser\":\"" + jsonEsc(ns_.mqttUser) + "\",\"mpass_set\":" +
       (ns_.mqttPass[0] ? "true" : "false") + ",\"mtopic\":\"" + jsonEsc(ns_.mqttTopic) + "\",\"mdisc\":" +
       String(ns_.mqttDiscovery) + ",\"admin_set\":" + (ns_.adminPass[0] ? "true" : "false");
  char xs[160];
  hkv::formatSchedule(xfer_, xs, sizeof(xs));
  j += String(",\"xfer\":\"") + xs + "\"";
  j += ",\"wmode\":" + String(pw_.wifiMode) + ",\"wps\":" + String(pw_.wifiPs) + ",\"wake\":" + String(pw_.wakeMin) +
       ",\"cpu\":" + String(pw_.cpuMhz) + ",\"lsleep\":" + String(pw_.lightSleep) + ",\"dinv\":" +
       String(s.displayInvert) + ",\"dflip\":" + String(s.displayFlip) + "}";
  sendJson(200, j);
}

void NetManager::handleConfigSave() {
  if (!requireAuth()) return;
  hkv::Settings s = app_.settings();
  String err;
  auto f = [this](const char* n, float cur) { return server_.arg(n).length() ? server_.arg(n).toFloat() : cur; };
  auto i = [this](const char* n, long cur) { return server_.arg(n).length() ? server_.arg(n).toInt() : cur; };

  s.meterId = static_cast<uint32_t>(i("meterId", s.meterId));
  if (server_.arg("mfct").length()) {
    String m = server_.arg("mfct");
    m.toUpperCase();
    copyStr(s.manufacturer, sizeof(s.manufacturer), m);
  }
  s.deviceVersion = static_cast<uint8_t>(i("ver", s.deviceVersion));
  s.scale = i("scale", static_cast<long>(s.scale)) == 1 ? hkv::ScaleMode::Product : hkv::ScaleMode::Unit;
  s.kc = f("kc", s.kc);
  s.kq = f("kq", s.kq);
  s.exponent = f("exp", s.exponent);
  s.nominalDeltaK = f("dtn", s.nominalDeltaK);
  s.startDeltaK = f("dts", s.startDeltaK);
  s.minRadiatorC = f("tmin", s.minRadiatorC);
  s.summerMinRadiatorC = f("tsum", s.summerMinRadiatorC);
  s.summerStartMonth = static_cast<uint8_t>(i("sumS", s.summerStartMonth));
  s.summerEndMonth = static_cast<uint8_t>(i("sumE", s.summerEndMonth));
  s.radiatorOffsetC = f("offR", s.radiatorOffsetC);
  s.roomOffsetC = f("offA", s.roomOffsetC);
  s.roomCoupling = f("rcpl", s.roomCoupling);
  s.roomCouplingTauMin = f("rtau", s.roomCouplingTauMin);
  s.emissivity = f("emis", s.emissivity);
  s.dueDay = static_cast<uint8_t>(i("dueD", s.dueDay));
  s.dueMonth = static_cast<uint8_t>(i("dueM", s.dueMonth));
  s.measureIntervalS = static_cast<uint16_t>(i("mint", s.measureIntervalS));
  s.telegramIntervalS = static_cast<uint16_t>(i("tint", s.telegramIntervalS));
  s.utcOffsetMin = static_cast<int16_t>(i("utco", s.utcOffsetMin));
  s.euDst = server_.hasArg("dst") ? 1 : 0;
  s.batteryLowMv = static_cast<uint16_t>(i("batlow", s.batteryLowMv));
  s.displayTimeoutS = static_cast<uint16_t>(i("dto", s.displayTimeoutS));
  s.displayInvert = server_.hasArg("dinv") ? 1 : 0;
  s.displayFlip = server_.hasArg("dflip") ? 1 : 0;
  s.omsSecurity = static_cast<uint8_t>(i("sec", s.omsSecurity));
  const String key = server_.arg("key");
  if (key.length() > 0 && !parseHexKey(key, s.aesKey)) err = "Der AES-Schlüssel muss aus 32 Hex-Zeichen bestehen.";
  bool keySet = false;
  for (uint8_t b : s.aesKey) keySet |= b != 0;
  if (err.isEmpty() && s.omsSecurity == 5 && !keySet) err = "Mode 5 braucht einen AES-Schlüssel.";

  NetSettings n = ns_;
  n.wifiEnabled = server_.hasArg("wifi") ? 1 : 0;
  if (server_.hasArg("ssid")) copyStr(n.ssid, sizeof(n.ssid), server_.arg("ssid"));
  if (server_.arg("pass").length()) copyStr(n.pass, sizeof(n.pass), server_.arg("pass"));
  if (server_.arg("host").length()) copyStr(n.hostname, sizeof(n.hostname), server_.arg("host"));
  if (server_.arg("ntp").length()) copyStr(n.ntpServer, sizeof(n.ntpServer), server_.arg("ntp"));
  if (server_.hasArg("noadmin")) {
    n.adminPass[0] = '\0';
  } else if (server_.arg("admin").length()) {
    if (server_.arg("admin").length() < 8) err = "Das Web-Passwort braucht mindestens 8 Zeichen.";
    else copyStr(n.adminPass, sizeof(n.adminPass), server_.arg("admin"));
  }
  n.mqttEnabled = server_.hasArg("mqtt") ? 1 : 0;
  n.mqttDiscovery = server_.hasArg("mdisc") ? 1 : 0;
  if (server_.hasArg("mhost")) copyStr(n.mqttHost, sizeof(n.mqttHost), server_.arg("mhost"));
  n.mqttPort = static_cast<uint16_t>(i("mport", n.mqttPort));
  if (server_.hasArg("muser")) copyStr(n.mqttUser, sizeof(n.mqttUser), server_.arg("muser"));
  if (server_.arg("mpass").length()) copyStr(n.mqttPass, sizeof(n.mqttPass), server_.arg("mpass"));
  if (server_.arg("mtopic").length()) copyStr(n.mqttTopic, sizeof(n.mqttTopic), server_.arg("mtopic"));
  if (err.isEmpty() && n.mqttEnabled && !n.mqttHost[0]) err = "MQTT ist aktiv, aber es ist kein Broker eingetragen.";
  if (err.isEmpty() && n.wifiEnabled && !n.ssid[0]) err = "Bitte ein WLAN auswählen oder eintragen.";
  hkv::TransferSchedule xfer = xfer_;
  if (err.isEmpty() && server_.hasArg("xfer") && hkv::parseSchedule(server_.arg("xfer").c_str(), xfer) != nullptr)
    err = "Übertragung: eine Zahl (Minuten, 1–1440) oder Uhrzeiten im Format hh:mm angeben, z. B. „06:00 18:00“.";

  if (err.isEmpty()) {
    const char* e = app_.applySettings(s);
    if (e) err = e;
  }
  if (!err.isEmpty()) {
    sendJson(400, jsonResult(false, err));
    return;
  }

  PowerSettings pw = pw_;
  pw.wifiMode = static_cast<uint8_t>(constrain(i("wmode", pw.wifiMode), 0L, 2L));
  pw.wifiPs = static_cast<uint8_t>(constrain(i("wps", pw.wifiPs), 0L, 2L));
  pw.wakeMin = static_cast<uint16_t>(constrain(i("wake", pw.wakeMin), 1L, 240L));
  const long cpu = i("cpu", pw.cpuMhz);
  pw.cpuMhz = static_cast<uint16_t>(cpu == 80 || cpu == 160 ? cpu : 240);
  pw.lightSleep = server_.hasArg("lsleep") ? 1 : 0;
  bool powerMsg = false;
  if (memcmp(&pw, &pw_, sizeof(pw)) != 0) {
    const bool modeChanged = pw.wifiMode != pw_.wifiMode;
    pw_ = pw;
    if (!hkv::saveObject(storage_, kKeyPower, kPowerVersion, pw_)) {
      sendJson(500, jsonResult(false, "Energie-Einstellungen konnten nicht gespeichert werden."));
      return;
    }
    // Changing the CPU clock while this request is still being answered can make the
    // response get lost (the browser then waits forever) - apply it shortly afterwards.
    powerApplyPending_ = true;
    powerApplyAt_ = millis() + 500;
    if (modeChanged && pw_.wifiMode != PowerSettings::Always && state_ != State::Off) {
      // keep the web UI reachable for the wake time instead of cutting the connection now
      wakeArmed_ = true;
      wakeUntil_ = millis() + pw_.wakeMin * 60000UL;
      powerMsg = true;
    } else if (modeChanged && pw_.wifiMode == PowerSettings::Always && state_ == State::Off && ns_.ssid[0]) {
      startStation(false);
    }
  }

  if (memcmp(&xfer, &xfer_, sizeof(xfer)) != 0) {  // takes effect immediately, no restart
    xfer_ = xfer;
    nextDueIdx_ = -1;
    if (!hkv::saveObject(storage_, kKeyXfer, kXferVersion, xfer_)) {
      sendJson(500, jsonResult(false, "Übertragungsplan konnte nicht gespeichert werden."));
      return;
    }
  }

  NetSettings withoutLogin = n;  // a changed web password needs no restart
  memcpy(withoutLogin.adminPass, ns_.adminPass, sizeof(withoutLogin.adminPass));
  const bool netChanged = memcmp(&withoutLogin, &ns_, sizeof(n)) != 0;
  const bool loginChanged = strcmp(n.adminPass, ns_.adminPass) != 0;
  if (netChanged || loginChanged) {
    ns_ = n;
    if (!saveSettings()) {
      sendJson(500, jsonResult(false, "Netzwerk-Einstellungen konnten nicht gespeichert werden."));
      return;
    }
  }
  if (netChanged) {
    restartPending_ = true;
    restartAt_ = millis() + 2000;
    sendJson(200, jsonResult(true, "Gespeichert – Neustart", true));
    return;
  }
  if (loginChanged) {
    sendJson(200, jsonResult(true, n.adminPass[0] ? "Gespeichert – Web-Passwort gesetzt" : "Gespeichert – Web-Passwort entfernt"));
    return;
  }
  if (powerMsg) {
    sendJson(200, jsonResult(true, "Gespeichert – WLAN bleibt noch " + String(pw_.wakeMin) +
                                       " min an, danach nur noch nach Plan bzw. per WLAN-Taste"));
    return;
  }
  sendJson(200, jsonResult(true, "Gespeichert"));
}

void NetManager::handleScan() {
  if (!requireAuth()) return;
  if (state_ != State::Portal && state_ != State::Connected) {
    sendJson(409, jsonResult(false, "Suche gerade nicht möglich"));
    return;
  }
  if (server_.hasArg("start")) {
    WiFi.scanDelete();
    WiFi.scanNetworks(true);  // asynchronous
    sendJson(200, "{\"scanning\":true}");
    return;
  }
  const int16_t n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) {
    sendJson(200, "{\"scanning\":true}");
    return;
  }
  if (n < 0) {
    sendJson(200, "{\"networks\":[]}");
    return;
  }
  // Strongest entry per SSID, sorted by signal.
  int idx[40];
  int count = 0;
  for (int k = 0; k < n && count < 40; ++k) {
    const String ssid = WiFi.SSID(k);
    if (!ssid.length()) continue;
    int dup = -1;
    for (int m = 0; m < count; ++m)
      if (WiFi.SSID(idx[m]) == ssid) dup = m;
    if (dup >= 0) {
      if (WiFi.RSSI(k) > WiFi.RSSI(idx[dup])) idx[dup] = k;
      continue;
    }
    idx[count++] = k;
  }
  for (int a = 0; a < count; ++a)
    for (int c = a + 1; c < count; ++c)
      if (WiFi.RSSI(idx[c]) > WiFi.RSSI(idx[a])) std::swap(idx[a], idx[c]);
  String j = "{\"networks\":[";
  for (int a = 0; a < count; ++a) {
    if (a) j += ',';
    j += "{\"ssid\":\"" + jsonEsc(WiFi.SSID(idx[a]).c_str()) + "\",\"rssi\":" + String(WiFi.RSSI(idx[a])) +
         ",\"secure\":" + (WiFi.encryptionType(idx[a]) == WIFI_AUTH_OPEN ? "false" : "true") + "}";
  }
  j += "]}";
  WiFi.scanDelete();
  sendJson(200, j);
}

// Live readings for the UI (every 2 s, display only - no influence on the units).
// Each call keeps live mode alive for 6 s, so it stops by itself when the page is closed.
void NetManager::handleLive() {
  if (server_.hasArg("stop")) {
    app_.stopLive();
    sendJson(200, "{\"ok\":true}");
    return;
  }
  app_.keepLiveAlive();
  const hkv::LiveValues& l = app_.live();
  String j = "{\"valid\":" + String(l.valid ? "true" : "false");
  j += ",\"radiator_c\":" + (l.radiatorRaw.valid ? fmtFloat(l.alloc.radiatorC, 2) : String("null"));
  j += ",\"room_c\":" + (l.roomRaw.valid ? fmtFloat(l.alloc.roomC, 2) : String("null"));
  j += ",\"delta_k\":" + fmtFloat(l.alloc.deltaK, 2);
  j += ",\"pressure_hpa\":" + (!isnan(l.roomRaw.pressureHpa) ? fmtFloat(l.roomRaw.pressureHpa, 1) : String("null"));
  j += ",\"humidity_pct\":" + (!isnan(l.roomRaw.humidityPct) ? fmtFloat(l.roomRaw.humidityPct, 1) : String("null"));
  j += ",\"age_ms\":" + String(millis() - l.uptimeMs) + ",\"interval_ms\":" + String(hkv::HkvApp::kLiveIntervalMs) + "}";
  sendJson(200, j);
}

void NetManager::handleSetTime() {
  if (!requireAuth()) return;
  const long utc = server_.arg("utc").toInt();
  if (utc < 1704067200L) {
    sendJson(400, jsonResult(false, "Ungültige Uhrzeit"));
    return;
  }
  app_.setTime(static_cast<uint32_t>(utc));
  sendJson(200, jsonResult(true, "Uhrzeit übernommen"));
}

void NetManager::handleAction() {
  if (!requireAuth()) return;
  const String what = server_.arg("do");
  if (what == "publish") {
    if (!mqtt_.connected()) {
      sendJson(409, jsonResult(false, "MQTT ist nicht verbunden"));
      return;
    }
    mqttPublishState();
    sendJson(200, jsonResult(true, "Übertragen"));
  } else if (what == "telegram") {
    app_.requestTelegram();
    sendJson(200, jsonResult(true, "Telegramm erzeugt (USB-Ausgabe, kein Funkmodul verbaut)"));
  } else if (what == "reboot") {
    restartPending_ = true;
    restartAt_ = millis() + 1000;
    sendJson(200, jsonResult(true, "Neustart"));
  } else if (what == "factory_reset") {
    if (server_.arg("confirm") != "yes") {
      sendJson(400, jsonResult(false, "Bestätigung fehlt"));
      return;
    }
    app_.eraseStoredData();
    storage_.erase(kKeyNet);
    storage_.erase(kKeyXfer);
    storage_.erase(kKeyPower);
    HKV_LOG("factory reset: network settings erased, restarting");
    restartPending_ = true;
    restartAt_ = millis() + 1500;
    sendJson(200, jsonResult(true, "Werkseinstellungen – Neustart"));
  } else if (what == "calibrate_coupling") {
    // c from a reference room temperature (thermometer); uses a fresh live reading.
    String v = server_.arg("ref");
    v.replace(",", ".");
    const float ref = v.toFloat();
    if (!v.length() || ref < 0.0f || ref > 40.0f) {
      sendJson(400, jsonResult(false, "Bitte die Raumtemperatur vom Thermometer eingeben (°C)"));
      return;
    }
    app_.keepLiveAlive();
    const hkv::LiveValues& l = app_.live();
    const hkv::Settings& st = app_.settings();
    if (!l.valid || !l.radiatorRaw.valid || !l.roomRaw.valid) {
      sendJson(409, jsonResult(false, "Sensoren liefern gerade keine gültigen Werte"));
      return;
    }
    // same lagged radiator value that the correction uses (see Settings::roomCouplingTauMin)
    const float lag = app_.radiatorLagC();
    const float rad = (isnan(lag) ? l.radiatorRaw.celsius : lag) + st.radiatorOffsetC;
    const float roomMeas = l.roomRaw.celsius + st.roomOffsetC;
    const float c = hkv::couplingFromReference(roomMeas, rad, ref);
    if (c < 0.0f) {
      sendJson(409, jsonResult(false, "Heizkörper zu kalt (" + fmtFloat(rad, 1) +
                                          " °C) – er muss mindestens 5 K wärmer als der Raum sein"));
      return;
    }
    if (c > 0.8f) {
      sendJson(409, jsonResult(false, "Kopplung " + fmtFloat(c, 2) +
                                          " ist unplausibel hoch – sitzt der Raumfühler direkt am Heizkörper?"));
      return;
    }
    char b[200];
    snprintf(b, sizeof(b), "{\"ok\":true,\"coupling\":%.2f,\"radiator_c\":%.1f,\"room_measured_c\":%.1f,\"reference_c\":%.1f}",
             static_cast<double>(c), static_cast<double>(rad), static_cast<double>(roomMeas), static_cast<double>(ref));
    sendJson(200, b);
  } else if (what == "reset_lifetime") {
    if (server_.arg("confirm") != "yes") {
      sendJson(400, jsonResult(false, "Bestätigung fehlt"));
      return;
    }
    app_.resetLifetime();
    sendJson(200, jsonResult(true, "Gesamtzähler zurückgesetzt"));
  } else if (what == "reset_current") {
    if (server_.arg("confirm") != "yes") {
      sendJson(400, jsonResult(false, "Bestätigung fehlt"));
      return;
    }
    app_.resetCurrentPeriod();
    sendJson(200, jsonResult(true, "Aktuelle Periode zurückgesetzt"));
  } else if (what == "delete_month") {
    const String m = server_.arg("month");  // YYYY-MM
    const int y = m.substring(0, 4).toInt();
    const int mo = m.substring(5, 7).toInt();
    if (m.length() != 7 || y < 2000 || mo < 1 || mo > 12 || server_.arg("confirm") != "yes") {
      sendJson(400, jsonResult(false, "Ungültiger Monat"));
      return;
    }
    if (app_.deleteMonthly(static_cast<uint16_t>(y), static_cast<uint8_t>(mo)))
      sendJson(200, jsonResult(true, "Monatswert gelöscht"));
    else
      sendJson(404, jsonResult(false, "Für diesen Monat ist kein Wert gespeichert"));
  } else if (what == "offset") {
    String v = server_.arg("value");
    v.replace(",", ".");
    v.trim();
    const double units = v.length() ? v.toDouble() : 0.0;
    if (units < 0 || units > 9999999) {
      sendJson(400, jsonResult(false, "Ungültiger Offset"));
      return;
    }
    const char* e = app_.setCounterOffset(static_cast<int64_t>(units * 1000.0 + 0.5));
    if (e) sendJson(400, jsonResult(false, e));
    else sendJson(200, jsonResult(true, units > 0 ? "Offset übernommen" : "Offset entfernt"));
  } else {
    sendJson(400, jsonResult(false, "Unbekannte Aktion"));
  }
}

void NetManager::statusJson(String& j) const {
  const hkv::Snapshot& sn = app_.snapshot();
  const hkv::MeterData& md = app_.meterData();
  const hkv::Settings& st = app_.settings();
  j.reserve(1900);
  char b[200];
  snprintf(b, sizeof(b), "{\"id\":\"%08lu\",\"manufacturer\":\"%s\",\"fw\":\"%s\",\"uptime_s\":%lu,\"measurements\":%lu,",
           static_cast<unsigned long>(st.meterId), st.manufacturer, HKV_FIRMWARE_VERSION,
           static_cast<unsigned long>(millis() / 1000), static_cast<unsigned long>(sn.measurements));
  j += b;
  hkv::DateTime lt;
  if (app_.localTime(lt)) {
    const hkv::DateTime ps = hkv::Ledger::periodStart(lt, st.dueMonth, st.dueDay);
    snprintf(b, sizeof(b),
             "\"local_time\":\"%04u-%02u-%02uT%02u:%02u:%02u\",\"dst\":%s,\"period_start\":\"%04u-%02u-%02u\",",
             lt.year, lt.month, lt.day, lt.hour, lt.minute, lt.second, lt.dst ? "true" : "false", ps.year, ps.month,
             ps.day);
  } else {
    snprintf(b, sizeof(b), "\"local_time\":null,\"period_start\":null,");
  }
  j += b;
  j += "\"units\":" + fmtUnits(md.currentMilliUnits) + ",\"units_current\":" + String(app_.currentUnits()) +
       ",\"units_lifetime\":" + fmtUnits(md.lifetimeMilliUnits) + ",\"measurements_total\":" +
       String(md.lifetimeMeasurements) + ",";
  if (md.dueDateYear) {
    snprintf(b, sizeof(b), "\"due_date\":\"%04u-%02u-%02u\",\"units_due_date\":%s,", md.dueDateYear,
             md.dueDateMonth, md.dueDateDay, fmtUnits(md.dueDateMilliUnits).c_str());
  } else {
    snprintf(b, sizeof(b), "\"due_date\":null,\"units_due_date\":null,");
  }
  j += b;
  j += "\"radiator_c\":" + (sn.radiatorRaw.valid ? fmtFloat(sn.alloc.radiatorC, 1) : String("null")) + ",";
  j += "\"room_c\":" + (sn.roomRaw.valid ? fmtFloat(sn.alloc.roomC, 1) : String("null")) + ",";
  j += "\"radiator_lag_c\":" +
       (!isnan(app_.radiatorLagC()) ? fmtFloat(app_.radiatorLagC() + st.radiatorOffsetC, 1) : String("null")) + ",";
  j += "\"room_raw_c\":" +
       (sn.roomRaw.valid ? fmtFloat(sn.roomRaw.celsius + st.roomOffsetC, 1) : String("null")) + ",";
  j += "\"pressure_hpa\":" + (!isnan(sn.roomRaw.pressureHpa) ? fmtFloat(sn.roomRaw.pressureHpa, 1) : String("null")) + ",";
  j += "\"humidity_pct\":" + (!isnan(sn.roomRaw.humidityPct) ? fmtFloat(sn.roomRaw.humidityPct, 1) : String("null")) + ",";
  snprintf(b, sizeof(b), "\"delta_k\":%.1f,\"rate_units_h\":%.2f,\"state\":\"%s\",\"room_fallback\":%s,",
           static_cast<double>(sn.alloc.deltaK), static_cast<double>(sn.alloc.unitsPerHour),
           hkv::countStateName(sn.alloc.state), sn.alloc.roomFallback ? "true" : "false");
  j += b;
  snprintf(b, sizeof(b),
           "\"battery_valid\":%s,\"battery_mv\":%u,\"battery_pct\":%u,\"charging\":%s,\"errors\":%lu,\"error_history\":%lu,",
           sn.battery.valid ? "true" : "false", sn.battery.millivolts, sn.battery.percent,
           sn.battery.charging ? "true" : "false", static_cast<unsigned long>(sn.errors),
           static_cast<unsigned long>(md.errorHistory));
  j += b;

  const char* mode = "off";
  switch (state_) {
    case State::Off: mode = "off"; break;
    case State::Connecting: mode = "connecting"; break;
    case State::Connected: mode = "connected"; break;
    case State::Failed: mode = "failed"; break;
    case State::Portal: mode = "ap"; break;
  }
  j += String("\"wifi\":{\"mode\":\"") + mode + "\",\"ssid\":\"" + jsonEsc(ns_.ssid) + "\",\"ap_ssid\":\"" +
       apSsid() + "\"";
  if (state_ == State::Connected) {
    j += ",\"ip\":\"" + WiFi.localIP().toString() + "\",\"rssi\":" + String(WiFi.RSSI()) +
         ",\"channel\":" + String(WiFi.channel());
  }
  j += "},";
  // rssi on top level for MQTT / Home Assistant
  j += "\"rssi\":" + (state_ == State::Connected ? String(WiFi.RSSI()) : String("null")) + ",";
  const char* mq = !ns_.mqttEnabled || !ns_.mqttHost[0] ? "disabled"
                   : const_cast<PubSubClient&>(mqtt_).connected() ? "connected"
                                                                   : "disconnected";
  j += String("\"mqtt\":\"") + mq + "\",";
  j += "\"power\":{\"wifi_mode\":" + String(pw_.wifiMode) + ",\"wake_left_s\":" +
       (wakeActive() ? String((wakeUntil_ - millis()) / 1000) : String("null")) + ",\"cpu_mhz\":" +
       String(getCpuFrequencyMhz()) + "},";
  {
    char xs[160];
    hkv::formatSchedule(xfer_, xs, sizeof(xs));
    NetManager* self = const_cast<NetManager*>(this);
    j += String("\"xfer\":{\"schedule\":\"") + xs + "\",\"last_s\":" +
         (mqttPublishedOnce_ ? String((millis() - mqttLastPublish_) / 1000) : String("null")) + ",\"next_s\":" +
         (!ns_.mqttEnabled || !ns_.mqttHost[0] ? String("null") : String(self->secondsToNextTransfer())) + "},";
  }
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(b, sizeof(b), "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  j += b;

  if (md.offsetMilliUnits != 0) {
    snprintf(b, sizeof(b), "\"offset\":{\"units\":%s,\"date\":",
             fmtUnits(static_cast<uint64_t>(md.offsetMilliUnits)).c_str());
    j += b;
    if (md.offsetYear) {
      snprintf(b, sizeof(b), "\"%04u-%02u-%02u\"},", md.offsetYear, md.offsetMonth, md.offsetDay);
      j += b;
    } else {
      j += F("null},");
    }
  } else {
    j += F("\"offset\":null,");
  }
  j += F("\"monthly\":[");
  bool first = true;
  for (int i = 0; i < hkv::kMonthlyHistory; ++i) {
    hkv::MonthlyValue mv;
    if (!app_.monthlyValue(i, mv)) continue;  // empty or deleted slot
    snprintf(b, sizeof(b), "%s{\"month\":\"%04u-%02u\",\"units\":%lu}", first ? "" : ",", mv.year, mv.month,
             static_cast<unsigned long>(mv.units));
    first = false;
    j += b;
  }
  size_t len;
  const uint8_t* tg = app_.lastTelegram(len);
  char hex[2 * hkv::oms::kMaxFrame + 1];
  hkv::oms::toHex(tg, len, hex, sizeof(hex));
  j += F("],\"telegram\":\"");
  j += hex;
  j += F("\"}");
}

void NetManager::handleApiStatus() {
  String j;
  statusJson(j);
  sendJson(200, j);
}

void NetManager::handleApiTelegram() {
  size_t len;
  const uint8_t* tg = app_.lastTelegram(len);
  char hex[2 * hkv::oms::kMaxFrame + 1];
  hkv::oms::toHex(tg, len, hex, sizeof(hex));
  server_.send(200, "text/plain", hex);
}

}  // namespace net

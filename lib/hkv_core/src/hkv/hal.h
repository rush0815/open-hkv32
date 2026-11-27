#pragma once
// Hardware abstraction layer of the HKV core.
//
// Everything in lib/hkv_core talks to the hardware ONLY through these interfaces.
// A port (ESP32 prototype today, TI CC1312 later) implements them and wires them
// together in its own main. Rules for the core, so it stays portable:
//   - no Arduino / ESP-IDF / TI driver headers
//   - no heap allocation, no exceptions, no RTTI, no std::string
//   - only <stdint.h>, <stddef.h>, <string.h>, <math.h>, <stdio.h>, <stdarg.h>

#include <math.h>
#include <stddef.h>
#include <stdint.h>

namespace hkv {

struct TempReading {
  bool valid = false;
  float celsius = 0.0f;
  // Optional extras of combined sensors (NAN = not available), e.g. BMP280 / BME280.
  float pressureHpa = NAN;
  float humidityPct = NAN;
};

class ITemperatureSensor {
 public:
  virtual ~ITemperatureSensor() = default;
  virtual bool begin() = 0;
  virtual TempReading read() = 0;
  virtual const char* name() const = 0;
  // Infrared sensors: emissivity of the measured surface (1.0 = no correction).
  // Contact sensors and the emulation ignore it.
  virtual void setEmissivity(float /*emissivity*/) {}
};

// Emissivity correction for thermopile/IR sensors whose own setting is 1.0.
// tMeasC: object temperature reported with emissivity 1, tAmbC: sensor (= reflected
// background) temperature. Radiation balance: To^4 = Ta^4 + (Tm^4 - Ta^4) / eps.
float correctEmissivity(float tMeasC, float tAmbC, float emissivity);

struct BatteryStatus {
  bool valid = false;
  uint16_t millivolts = 0;
  uint8_t percent = 0;      // 0..100, chemistry specific estimate of the port
  bool charging = false;    // only meaningful for rechargeable prototypes
};

class IBattery {
 public:
  virtual ~IBattery() = default;
  virtual bool begin() = 0;
  virtual BatteryStatus read() = 0;
};

class IClock {
 public:
  virtual ~IClock() = default;
  // Seconds since 1970-01-01 UTC. Only meaningful when isTimeValid().
  virtual uint32_t nowUtc() = 0;
  virtual bool isTimeValid() = 0;
  virtual void setUtc(uint32_t utc) = 0;
  // Monotonic milliseconds since boot (must keep counting during sleep).
  virtual uint32_t uptimeMs() = 0;
};

// Small key/value store for settings and meter data (NVS on ESP32, NVS driver on CC1312).
class IStorage {
 public:
  virtual ~IStorage() = default;
  virtual bool read(const char* key, void* buf, size_t len) = 0;
  virtual bool write(const char* key, const void* buf, size_t len) = 0;
  virtual bool erase(const char* key) = 0;
};

class IRandom {
 public:
  virtual ~IRandom() = default;
  virtual uint32_t next() = 0;
  // Uniform float in [0, 1).
  float uniform() { return static_cast<float>(next() >> 8) / 16777216.0f; }
};

// AES-128 single block encryption (ECB primitive). CBC etc. is done by the core.
// The core ships a software implementation (aes128.h); a port may use hardware AES.
class IBlockCipher {
 public:
  virtual ~IBlockCipher() = default;
  virtual void setKey(const uint8_t key[16]) = 0;
  virtual void encryptBlock(const uint8_t in[16], uint8_t out[16]) = 0;
};

// Receives finished wM-Bus/OMS link-layer frames (L-field first, WITHOUT block CRCs).
// Prototype: serial/web output. CC1312: RF driver (adds CRCs / 3-of-6 coding as needed).
class ITelegramSink {
 public:
  virtual ~ITelegramSink() = default;
  virtual void send(const uint8_t* frame, size_t len) = 0;
};

// Temporary working memory, e.g. the samples of a calibration run (a few KB for a few hours).
// The core itself never uses the heap; it asks the port and gives the memory back right after.
class IMemory {
 public:
  virtual ~IMemory() = default;
  virtual void* acquire(size_t bytes) = 0;  // nullptr = not available
  virtual void release(void* p) = 0;
};

struct DisplayModel;  // display_model.h

class IDisplay {
 public:
  virtual ~IDisplay() = default;
  virtual bool begin() = 0;
  virtual void show(const DisplayModel& model) = 0;
  virtual void off() = 0;
  // Panel options from the settings (colour inversion, 180 degree rotation).
  virtual void configure(bool /*invert*/, bool /*flip*/) {}
};

}  // namespace hkv
